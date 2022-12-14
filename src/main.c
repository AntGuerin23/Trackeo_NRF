#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/drivers/uart.h>
#include <string.h>
#include <zephyr/random/rand32.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#if defined(CONFIG_NRF_MODEM_LIB)
#include <nrf_modem_at.h>
#endif /* CONFIG_NRF_MODEM_LIB */
#include <modem/lte_lc.h>
#include <zephyr/logging/log.h>
#if defined(CONFIG_MODEM_KEY_MGMT)
#include <modem/modem_key_mgmt.h>
#endif
#if defined(CONFIG_LWM2M_CARRIER)
#include <lwm2m_carrier.h>
#endif
#include <dk_buttons_and_leds.h>
#include <stdlib.h>
#include <math.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrf_modem_gnss.h>
#include <date_time.h>

// LOG_MODULE_REGISTER(gnss_sample, CONFIG_GNSS_SAMPLE_LOG_LEVEL);

#define PI 3.14159265358979323846
#define EARTH_RADIUS_METERS (6371.0 * 1000.0)

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE) || defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
static struct k_work_q gnss_work_q;

#define GNSS_WORKQ_THREAD_STACK_SIZE 2304
#define GNSS_WORKQ_THREAD_PRIORITY 5

K_THREAD_STACK_DEFINE(gnss_workq_stack_area, GNSS_WORKQ_THREAD_STACK_SIZE);
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE || CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
#include "assistance.h"

static struct nrf_modem_gnss_agps_data_frame last_agps;
static struct k_work agps_data_get_work;
static volatile bool requesting_assistance;
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE */

#if defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
static struct k_work_delayable ttff_test_got_fix_work;
static struct k_work_delayable ttff_test_prepare_work;
static struct k_work ttff_test_start_work;
static uint32_t time_to_fix;
#endif

#include "certificates.h"

LOG_MODULE_REGISTER(mqtt_simple, CONFIG_MQTT_SIMPLE_LOG_LEVEL);

/* Buffers for MQTT client. */
static uint8_t rx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t tx_buffer[CONFIG_MQTT_MESSAGE_BUFFER_SIZE];
static uint8_t payload_buf[CONFIG_MQTT_PAYLOAD_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

/* File descriptor */
static struct pollfd fds;

static const char update_indicator[] = {'\\', '|', '/', '-'};

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static uint64_t fix_timestamp;
static uint32_t time_blocked;

/* Reference position. */
static bool ref_used;
static double ref_latitude;
static double ref_longitude;

static double last_latitude = 0;
static double last_longitude = 0;

K_MSGQ_DEFINE(nmea_queue, sizeof(struct nrf_modem_gnss_nmea_data_frame *), 10, 4);
static K_SEM_DEFINE(pvt_data_sem, 0, 1);
static K_SEM_DEFINE(time_sem, 0, 1);

static K_SEM_DEFINE(gps_sem, 0, 1);
static K_SEM_DEFINE(lte_sem, 0, 1);


//gps semaphore
//lte semaphore

static struct k_poll_event events[2] = {
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE,
									K_POLL_MODE_NOTIFY_ONLY,
									&pvt_data_sem, 0),
	K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
									K_POLL_MODE_NOTIFY_ONLY,
									&nmea_queue, 0),
};

BUILD_ASSERT(IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_GPS) ||
				 IS_ENABLED(CONFIG_LTE_NETWORK_MODE_NBIOT_GPS) ||
				 IS_ENABLED(CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS),
			 "CONFIG_LTE_NETWORK_MODE_LTE_M_GPS, "
			 "CONFIG_LTE_NETWORK_MODE_NBIOT_GPS or "
			 "CONFIG_LTE_NETWORK_MODE_LTE_M_NBIOT_GPS must be enabled");

BUILD_ASSERT((sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LATITUDE) == 1 &&
			  sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LONGITUDE) == 1) ||
				 (sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LATITUDE) > 1 &&
				  sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LONGITUDE) > 1),
			 "CONFIG_GNSS_SAMPLE_REFERENCE_LATITUDE and "
			 "CONFIG_GNSS_SAMPLE_REFERENCE_LONGITUDE must be both either set or empty");

#if defined(CONFIG_MQTT_LIB_TLS)
static int certificates_provision(void)
{
	int err = 0;

	LOG_INF("Provisioning certificates");

#if defined(CONFIG_NRF_MODEM_LIB) && defined(CONFIG_MODEM_KEY_MGMT)

	err = modem_key_mgmt_write(CONFIG_MQTT_TLS_SEC_TAG,
				   MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				   CA_CERTIFICATE,
				   strlen(CA_CERTIFICATE));
	if (err) {
		LOG_ERR("Failed to provision CA certificate: %d", err);
		return err;
	}

#elif defined(CONFIG_BOARD_QEMU_X86) && defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)

	err = tls_credential_add(CONFIG_MQTT_TLS_SEC_TAG,
				 TLS_CREDENTIAL_CA_CERTIFICATE,
				 CA_CERTIFICATE,
				 sizeof(CA_CERTIFICATE));
	if (err) {
		LOG_ERR("Failed to register CA certificate: %d", err);
		return err;
	}

#endif

	return err;
}
#endif /* defined(CONFIG_MQTT_LIB_TLS) */

#if defined(CONFIG_LWM2M_CARRIER)
K_SEM_DEFINE(carrier_registered, 0, 1);
int lwm2m_carrier_event_handler(const lwm2m_carrier_event_t *event)
{
	switch (event->type) {
	case LWM2M_CARRIER_EVENT_BSDLIB_INIT:
		LOG_INF("LWM2M_CARRIER_EVENT_BSDLIB_INIT");
		break;
	case LWM2M_CARRIER_EVENT_CONNECTING:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECTING");
		break;
	case LWM2M_CARRIER_EVENT_CONNECTED:
		LOG_INF("LWM2M_CARRIER_EVENT_CONNECTED");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECTING:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECTING");
		break;
	case LWM2M_CARRIER_EVENT_DISCONNECTED:
		LOG_INF("LWM2M_CARRIER_EVENT_DISCONNECTED");
		break;
	case LWM2M_CARRIER_EVENT_BOOTSTRAPPED:
		LOG_INF("LWM2M_CARRIER_EVENT_BOOTSTRAPPED");
		break;
	case LWM2M_CARRIER_EVENT_REGISTERED:
		LOG_INF("LWM2M_CARRIER_EVENT_REGISTERED");
		k_sem_give(&carrier_registered);
		break;
	case LWM2M_CARRIER_EVENT_DEFERRED:
		LOG_INF("LWM2M_CARRIER_EVENT_DEFERRED");
		break;
	case LWM2M_CARRIER_EVENT_FOTA_START:
		LOG_INF("LWM2M_CARRIER_EVENT_FOTA_START");
		break;
	case LWM2M_CARRIER_EVENT_REBOOT:
		LOG_INF("LWM2M_CARRIER_EVENT_REBOOT");
		break;
	case LWM2M_CARRIER_EVENT_ERROR:
		LOG_ERR("LWM2M_CARRIER_EVENT_ERROR: code %d, value %d",
			((lwm2m_carrier_event_error_t *)event->data)->code,
			((lwm2m_carrier_event_error_t *)event->data)->value);
		break;
	default:
		LOG_WRN("Unhandled LWM2M_CARRIER_EVENT: %d", event->type);
		break;
	}

	return 0;
}
#endif /* defined(CONFIG_LWM2M_CARRIER) */

/**@brief Function to print strings without null-termination
 */
static void data_print(uint8_t *prefix, uint8_t *data, size_t len)
{
	char buf[len + 1];

	memcpy(buf, data, len);
	buf[len] = 0;
	LOG_INF("%s%s", log_strdup(prefix), log_strdup(buf));
}

/**@brief Function to publish data on the configured topic
 */
static int data_publish(struct mqtt_client *c, enum mqtt_qos qos,
	uint8_t *data, size_t len)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = CONFIG_MQTT_PUB_TOPIC;
	param.message.topic.topic.size = strlen(CONFIG_MQTT_PUB_TOPIC);
	param.message.payload.data = data;
	param.message.payload.len = len;
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	data_print("Publishing: ", data, len);
	LOG_INF("to topic: %s len: %u",
		CONFIG_MQTT_PUB_TOPIC,
		(unsigned int)strlen(CONFIG_MQTT_PUB_TOPIC));

	return mqtt_publish(c, &param);
}

/**@brief Function to subscribe to the configured topic
 */
static int subscribe(void)
{
	struct mqtt_topic subscribe_topic = {
		.topic = {
			.utf8 = CONFIG_MQTT_SUB_TOPIC,
			.size = strlen(CONFIG_MQTT_SUB_TOPIC)
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE
	};

	const struct mqtt_subscription_list subscription_list = {
		.list = &subscribe_topic,
		.list_count = 1,
		.message_id = 1234
	};

	LOG_INF("Subscribing to: %s len %u", CONFIG_MQTT_SUB_TOPIC,
		(unsigned int)strlen(CONFIG_MQTT_SUB_TOPIC));

	return mqtt_subscribe(&client, &subscription_list);
}

/**@brief Function to read the published payload.
 */
static int publish_get_payload(struct mqtt_client *c, size_t length)
{
	int ret;
	int err = 0;

	/* Return an error if the payload is larger than the payload buffer.
	 * Note: To allow new messages, we have to read the payload before returning.
	 */
	if (length > sizeof(payload_buf)) {
		err = -EMSGSIZE;
	}

	/* Truncate payload until it fits in the payload buffer. */
	while (length > sizeof(payload_buf)) {
		ret = mqtt_read_publish_payload_blocking(
				c, payload_buf, (length - sizeof(payload_buf)));
		if (ret == 0) {
			return -EIO;
		} else if (ret < 0) {
			return ret;
		}

		length -= ret;
	}

	ret = mqtt_readall_publish_payload(c, payload_buf, length);
	if (ret) {
		return ret;
	}

	return err;
}

/**@brief MQTT client event handler
 */
void mqtt_evt_handler(struct mqtt_client *const c,
		      const struct mqtt_evt *evt)
{
	int err;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT connect failed: %d", evt->result);
			break;
		}

		LOG_INF("MQTT client connected");
		subscribe();
		break;

	case MQTT_EVT_DISCONNECT:
		LOG_INF("MQTT client disconnected: %d", evt->result);
		break;

	case MQTT_EVT_PUBLISH: {
		const struct mqtt_publish_param *p = &evt->param.publish;

		LOG_INF("MQTT PUBLISH result=%d len=%d",
			evt->result, p->message.payload.len);
		err = publish_get_payload(c, p->message.payload.len);

		if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
			const struct mqtt_puback_param ack = {
				.message_id = p->message_id
			};

			/* Send acknowledgment. */
			mqtt_publish_qos1_ack(&client, &ack);
		}

		if (err >= 0) {
			data_print("Received: ", payload_buf, // Received on /ping
					   p->message.payload.len);

			char message[30];
			snprintf(message, 30, "%3.7f,%3.7f", last_latitude, last_longitude);

			//TODO : Trim array

			data_publish(&client, MQTT_QOS_1_AT_LEAST_ONCE, // Publish on /tracker
						 message, 30);

			printf("Releasing gps \n");
			k_sem_give(&gps_sem);

			printf("Stopping lte (myself) \n");
			k_sem_take(&lte_sem, K_FOREVER);
		} else if (err == -EMSGSIZE) {
			LOG_ERR("Received payload (%d bytes) is larger than the payload buffer "
				"size (%d bytes).",
				p->message.payload.len, sizeof(payload_buf));
		} else {
			LOG_ERR("publish_get_payload failed: %d", err);
			LOG_INF("Disconnecting MQTT client...");

			err = mqtt_disconnect(c);
			if (err) {
				LOG_ERR("Could not disconnect: %d", err);
			}
		}
	} break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT PUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("PUBACK packet id: %u", evt->param.puback.message_id);
		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			LOG_ERR("MQTT SUBACK error: %d", evt->result);
			break;
		}

		LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
		break;

	case MQTT_EVT_PINGRESP:
		if (evt->result != 0) {
			LOG_ERR("MQTT PINGRESP error: %d", evt->result);
		}
		break;

	default:
		LOG_INF("Unhandled MQTT event type: %d", evt->type);
		break;
	}
}

/**@brief Resolves the configured hostname and
 * initializes the MQTT broker structure
 */
static int broker_init(void)
{
	int err;
	struct addrinfo *result;
	struct addrinfo *addr;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM
	};

	err = getaddrinfo(CONFIG_MQTT_BROKER_HOSTNAME, NULL, &hints, &result);
	if (err) {
		LOG_ERR("getaddrinfo failed: %d", err);
		return -ECHILD;
	}

	addr = result;

	/* Look for address of the broker. */
	while (addr != NULL) {
		/* IPv4 Address. */
		if (addr->ai_addrlen == sizeof(struct sockaddr_in)) {
			struct sockaddr_in *broker4 =
				((struct sockaddr_in *)&broker);
			char ipv4_addr[NET_IPV4_ADDR_LEN];

			broker4->sin_addr.s_addr =
				((struct sockaddr_in *)addr->ai_addr)
				->sin_addr.s_addr;
			broker4->sin_family = AF_INET;
			broker4->sin_port = htons(CONFIG_MQTT_BROKER_PORT);

			inet_ntop(AF_INET, &broker4->sin_addr.s_addr,
				  ipv4_addr, sizeof(ipv4_addr));
			LOG_INF("IPv4 Address found %s", log_strdup(ipv4_addr));

			break;
		} else {
			LOG_ERR("ai_addrlen = %u should be %u or %u",
				(unsigned int)addr->ai_addrlen,
				(unsigned int)sizeof(struct sockaddr_in),
				(unsigned int)sizeof(struct sockaddr_in6));
		}

		addr = addr->ai_next;
	}

	/* Free the address. */
	freeaddrinfo(result);

	return err;
}

#if defined(CONFIG_NRF_MODEM_LIB)
#define IMEI_LEN 15
#define CGSN_RESPONSE_LENGTH (IMEI_LEN + 6 + 1) /* Add 6 for \r\nOK\r\n and 1 for \0 */
#define CLIENT_ID_LEN sizeof("nrf-") + IMEI_LEN
#else
#define RANDOM_LEN 10
#define CLIENT_ID_LEN sizeof(CONFIG_BOARD) + 1 + RANDOM_LEN
#endif /* defined(CONFIG_NRF_MODEM_LIB) */

/* Function to get the client id */
static const uint8_t* client_id_get(void)
{
	static uint8_t client_id[MAX(sizeof(CONFIG_MQTT_CLIENT_ID),
				     CLIENT_ID_LEN)];

	if (strlen(CONFIG_MQTT_CLIENT_ID) > 0) {
		snprintf(client_id, sizeof(client_id), "%s",
			 CONFIG_MQTT_CLIENT_ID);
		goto exit;
	}

#if defined(CONFIG_NRF_MODEM_LIB)
	char imei_buf[CGSN_RESPONSE_LENGTH + 1];
	int err;

	err = nrf_modem_at_cmd(imei_buf, sizeof(imei_buf), "AT+CGSN");
	if (err) {
		LOG_ERR("Failed to obtain IMEI, error: %d", err);
		goto exit;
	}

	imei_buf[IMEI_LEN] = '\0';

	snprintf(client_id, sizeof(client_id), "nrf-%.*s", IMEI_LEN, imei_buf);
#else
	uint32_t id = sys_rand32_get();
	snprintf(client_id, sizeof(client_id), "%s-%010u", CONFIG_BOARD, id);
#endif /* !defined(NRF_CLOUD_CLIENT_ID) */

exit:
	LOG_DBG("client_id = %s", log_strdup(client_id));

	return client_id;
}

/**@brief Initialize the MQTT client structure
 */
static int client_init(struct mqtt_client *client)
{
	int err;

	mqtt_client_init(client);

	err = broker_init();
	if (err) {
		LOG_ERR("Failed to initialize broker connection");
		return err;
	}

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = client_id_get();
	client->client_id.size = strlen(client->client_id.utf8);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
	struct mqtt_sec_config *tls_cfg = &(client->transport).tls.config;
	static sec_tag_t sec_tag_list[] = { CONFIG_MQTT_TLS_SEC_TAG };

	LOG_INF("TLS enabled");
	client->transport.type = MQTT_TRANSPORT_SECURE;

	tls_cfg->peer_verify = CONFIG_MQTT_TLS_PEER_VERIFY;
	tls_cfg->cipher_count = 0;
	tls_cfg->cipher_list = NULL;
	tls_cfg->sec_tag_count = ARRAY_SIZE(sec_tag_list);
	tls_cfg->sec_tag_list = sec_tag_list;
	tls_cfg->hostname = CONFIG_MQTT_BROKER_HOSTNAME;

#if defined(CONFIG_NRF_MODEM_LIB)
	tls_cfg->session_cache = IS_ENABLED(CONFIG_MQTT_TLS_SESSION_CACHING) ?
					    TLS_SESSION_CACHE_ENABLED :
					    TLS_SESSION_CACHE_DISABLED;
#else
	/* TLS session caching is not supported by the Zephyr network stack */
	tls_cfg->session_cache = TLS_SESSION_CACHE_DISABLED;

#endif

#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif

	return err;
}

/**@brief Initialize the file descriptor structure used by poll.
 */
static int fds_init(struct mqtt_client *c)
{
	if (c->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds.fd = c->transport.tcp.sock;
	} else {
#if defined(CONFIG_MQTT_LIB_TLS)
		fds.fd = c->transport.tls.sock;
#else
		return -ENOTSUP;
#endif
	}

	fds.events = POLLIN;

	return 0;
}

#if defined(CONFIG_DK_LIBRARY)
static void button_handler(uint32_t button_states, uint32_t has_changed)
{
	if (has_changed & button_states &
		BIT(CONFIG_BUTTON_EVENT_BTN_NUM - 1))
	{
		int ret;

		char message[] = "Bonjour";

		ret = data_publish(&client,
						   MQTT_QOS_1_AT_LEAST_ONCE,
						   message,
						   sizeof(message) - 1);
		if (ret)
		{
			LOG_ERR("Publish failed: %d", ret);
		}
	}
}
#endif

/**@brief Configures modem to provide LTE link. Blocks until link is
 * successfully established.
 */
static int modem_configure(void)
{
	int err = 0;
	LOG_INF("Turning on EDRX");
	//lte_lc_edrx_req(true);
	char at_buf[100];
	err = nrf_modem_at_cmd(at_buf, sizeof(at_buf), "AT+CEDRXS=2,4,\"0010\"");
	if (err != 0 ) {
		printf("%s", at_buf);
		printf("ERROR EDRX-------------------------------------------");
	}
#if defined(CONFIG_LTE_LINK_CONTROL)
	/* Turn off LTE power saving features for a more responsive demo. Also,
	 * request power saving features before network registration. Some
	 * networks rejects timer updates after the device has registered to the
	 * LTE network.
	 */
	// LOG_INF("Disabling PSM and eDRX");
	// lte_lc_psm_req(false);

	if (IS_ENABLED(CONFIG_LTE_AUTO_INIT_AND_CONNECT)) {
		/* Do nothing, modem is already turned on
		 * and connected.
		 */
	} else {
#if defined(CONFIG_LWM2M_CARRIER)
		/* Wait for the LWM2M_CARRIER to configure the modem and
		 * start the connection.
		 */
		LOG_INF("Waitng for carrier registration...");
		k_sem_take(&carrier_registered, K_FOREVER);
		LOG_INF("Registered!");
#else /* defined(CONFIG_LWM2M_CARRIER) */
		int err;

		LOG_INF("LTE Link Connecting...");
		err = lte_lc_init_and_connect();
		if (err) {
			LOG_INF("Failed to establish LTE connection: %d", err);
			return err;
		}
		LOG_INF("LTE Link Connected!");
#endif /* defined(CONFIG_LWM2M_CARRIER) */
	}
#endif /* defined(CONFIG_LTE_LINK_CONTROL) */

	return 0;
}


//--------------------- GNSS

static void gnss_event_handler(int event)
{
	int retval;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;

	switch (event)
	{
	case NRF_MODEM_GNSS_EVT_PVT:
		retval = nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt), NRF_MODEM_GNSS_DATA_PVT);
		if (retval == 0)
		{
			k_sem_give(&pvt_data_sem);
		}
		break;

#if defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
	case NRF_MODEM_GNSS_EVT_FIX:
		/* Time to fix is calculated here, but it's printed from a delayed work to avoid
		 * messing up the NMEA output.
		 */
		time_to_fix = (k_uptime_get() - fix_timestamp) / 1000;
		k_work_schedule_for_queue(&gnss_work_q, &ttff_test_got_fix_work, K_MSEC(100));
		k_work_schedule_for_queue(&gnss_work_q, &ttff_test_prepare_work,
								  K_SECONDS(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST_INTERVAL));
		break;
#endif /* CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */

	case NRF_MODEM_GNSS_EVT_NMEA:
		nmea_data = k_malloc(sizeof(struct nrf_modem_gnss_nmea_data_frame));
		if (nmea_data == NULL)
		{
			LOG_ERR("Failed to allocate memory for NMEA");
			break;
		}

		retval = nrf_modem_gnss_read(nmea_data,
									 sizeof(struct nrf_modem_gnss_nmea_data_frame),
									 NRF_MODEM_GNSS_DATA_NMEA);
		if (retval == 0)
		{
			retval = k_msgq_put(&nmea_queue, &nmea_data, K_NO_WAIT);
		}

		if (retval != 0)
		{
			k_free(nmea_data);
		}
		break;

	case NRF_MODEM_GNSS_EVT_AGPS_REQ:
#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
		retval = nrf_modem_gnss_read(&last_agps,
									 sizeof(last_agps),
									 NRF_MODEM_GNSS_DATA_AGPS_REQ);
		if (retval == 0)
		{
			k_work_submit_to_queue(&gnss_work_q, &agps_data_get_work);
		}
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE */
		break;

	default:
		break;
	}
}

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
#if defined(CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND)
K_SEM_DEFINE(lte_ready, 0, 1);

static void lte_lc_event_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type)
	{
	case LTE_LC_EVT_NW_REG_STATUS:
		if ((evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME) ||
			(evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING))
		{
			LOG_INF("Connected to LTE network");
			k_sem_give(&lte_ready);
		}
		break;

	default:
		break;
	}
}

void lte_connect(void)
{
	int err;

	LOG_INF("Connecting to LTE network");

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_LTE);
	if (err)
	{
		LOG_ERR("Failed to activate LTE, error: %d", err);
		return;
	}

	k_sem_take(&lte_ready, K_FOREVER);

	/* Wait for a while, because with IPv4v6 PDN the IPv6 activation takes a bit more time. */
	k_sleep(K_SECONDS(1));
}

void lte_disconnect(void)
{
	int err;

	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE);
	if (err)
	{
		LOG_ERR("Failed to deactivate LTE, error: %d", err);
		return;
	}

	LOG_INF("LTE disconnected");
}
#endif /* CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND */

static void agps_data_get_work_fn(struct k_work *item)
{
	ARG_UNUSED(item);

	int err;

#if defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_SUPL)
	/* SUPL doesn't usually provide satellite real time integrity information. If GNSS asks
	 * only for satellite integrity, the request should be ignored.
	 */
	if (last_agps.sv_mask_ephe == 0 &&
		last_agps.sv_mask_alm == 0 &&
		last_agps.data_flags == NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST)
	{
		LOG_INF("Ignoring assistance request for only satellite integrity");
		return;
	}
#endif /* CONFIG_GNSS_SAMPLE_ASSISTANCE_SUPL */

#if defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_MINIMAL)
	/* With minimal assistance, the request should be ignored if no GPS time or position
	 * is requested.
	 */
	if (!(last_agps.data_flags & NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST) &&
		!(last_agps.data_flags & NRF_MODEM_GNSS_AGPS_POSITION_REQUEST))
	{
		LOG_INF("Ignoring assistance request because no GPS time or position is requested");
		return;
	}
#endif /* CONFIG_GNSS_SAMPLE_ASSISTANCE_MINIMAL */

	requesting_assistance = true;

	LOG_INF("Assistance data needed, ephe 0x%08x, alm 0x%08x, flags 0x%02x",
			last_agps.sv_mask_ephe,
			last_agps.sv_mask_alm,
			last_agps.data_flags);

#if defined(CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND)
	lte_connect();
#endif /* CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND */

	err = assistance_request(&last_agps);
	if (err)
	{
		LOG_ERR("Failed to request assistance data");
	}

#if defined(CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND)
	lte_disconnect();
#endif /* CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND */

	requesting_assistance = false;
}
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE */

#if defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
static void ttff_test_got_fix_work_fn(struct k_work *item)
{
	LOG_INF("Time to fix: %u", time_to_fix);
	if (time_blocked > 0)
	{
		LOG_INF("Time GNSS was blocked by LTE: %u", time_blocked);
	}
	//print_distance_from_reference(&last_pvt);
	LOG_INF("Sleeping for %u seconds", CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST_INTERVAL);
}

static int ttff_test_force_cold_start(void)
{
	int err;
	uint32_t delete_mask;

	LOG_INF("Deleting GNSS data");

	/* Delete everything else except the TCXO offset. */
	delete_mask = NRF_MODEM_GNSS_DELETE_EPHEMERIDES |
				  NRF_MODEM_GNSS_DELETE_ALMANACS |
				  NRF_MODEM_GNSS_DELETE_IONO_CORRECTION_DATA |
				  NRF_MODEM_GNSS_DELETE_LAST_GOOD_FIX |
				  NRF_MODEM_GNSS_DELETE_GPS_TOW |
				  NRF_MODEM_GNSS_DELETE_GPS_WEEK |
				  NRF_MODEM_GNSS_DELETE_UTC_DATA |
				  NRF_MODEM_GNSS_DELETE_GPS_TOW_PRECISION;

	/* With minimal assistance, we want to keep the factory almanac. */
	if (IS_ENABLED(CONFIG_GNSS_SAMPLE_ASSISTANCE_MINIMAL))
	{
		delete_mask &= ~NRF_MODEM_GNSS_DELETE_ALMANACS;
	}

	err = nrf_modem_gnss_nv_data_delete(delete_mask);
	if (err)
	{
		LOG_ERR("Failed to delete GNSS data");
		return -1;
	}

	return 0;
}

static void ttff_test_prepare_work_fn(struct k_work *item)
{
	/* Make sure GNSS is stopped before next start. */
	nrf_modem_gnss_stop();

	if (IS_ENABLED(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST_COLD_START))
	{
		if (ttff_test_force_cold_start() != 0)
		{
			return;
		}
	}

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
	if (IS_ENABLED(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST_COLD_START))
	{
		/* All A-GPS data is always requested before GNSS is started. */
		last_agps.sv_mask_ephe = 0xffffffff;
		last_agps.sv_mask_alm = 0xffffffff;
		last_agps.data_flags =
			NRF_MODEM_GNSS_AGPS_GPS_UTC_REQUEST |
			NRF_MODEM_GNSS_AGPS_KLOBUCHAR_REQUEST |
			NRF_MODEM_GNSS_AGPS_SYS_TIME_AND_SV_TOW_REQUEST |
			NRF_MODEM_GNSS_AGPS_POSITION_REQUEST |
			NRF_MODEM_GNSS_AGPS_INTEGRITY_REQUEST;

		k_work_submit_to_queue(&gnss_work_q, &agps_data_get_work);
	}
	else
	{
		/* Start and stop GNSS to trigger possible A-GPS data request. If new A-GPS
		 * data is needed it is fetched before GNSS is started.
		 */
		nrf_modem_gnss_start();
		nrf_modem_gnss_stop();
	}
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE */

	k_work_submit_to_queue(&gnss_work_q, &ttff_test_start_work);
}

static void ttff_test_start_work_fn(struct k_work *item)
{
	LOG_INF("Starting GNSS");
	if (nrf_modem_gnss_start() != 0)
	{
		LOG_ERR("Failed to start GNSS");
		return;
	}

	fix_timestamp = k_uptime_get();
	time_blocked = 0;
}
#endif /* CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	k_sem_give(&time_sem);
}

static int modem_init(void)
{
	if (IS_ENABLED(CONFIG_DATE_TIME))
	{
		date_time_register_handler(date_time_evt_handler);
	}

	if (lte_lc_init() != 0)
	{
		LOG_ERR("Failed to initialize LTE link controller");
		return -1;
	}

#if defined(CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND)
	lte_lc_register_handler(lte_lc_event_handler);
#elif !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
	lte_lc_psm_req(true);

	LOG_INF("Connecting to LTE network");

	if (lte_lc_connect() != 0)
	{
		LOG_ERR("Failed to connect to LTE network");
		return -1;
	}

	LOG_INF("Connected to LTE network");

	if (IS_ENABLED(CONFIG_DATE_TIME))
	{
		LOG_INF("Waiting for current time");

		/* Wait for an event from the Date Time library. */
		k_sem_take(&time_sem, K_MINUTES(10));

		if (!date_time_is_valid())
		{
			LOG_WRN("Failed to get current time, continuing anyway");
		}
	}
#endif

	return 0;
}

static int sample_init(void)
{
	int err = 0;

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE) || defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
	struct k_work_queue_config cfg = {
		.name = "gnss_work_q",
		.no_yield = false};

	k_work_queue_start(
		&gnss_work_q,
		gnss_workq_stack_area,
		K_THREAD_STACK_SIZEOF(gnss_workq_stack_area),
		GNSS_WORKQ_THREAD_PRIORITY,
		&cfg);
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE || CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */

#if !defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE)
	k_work_init(&agps_data_get_work, agps_data_get_work_fn);

	err = assistance_init(&gnss_work_q);
#endif /* !CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE */

#if defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
	k_work_init_delayable(&ttff_test_got_fix_work, ttff_test_got_fix_work_fn);
	k_work_init_delayable(&ttff_test_prepare_work, ttff_test_prepare_work_fn);
	k_work_init(&ttff_test_start_work, ttff_test_start_work_fn);
#endif /* CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */

	return err;
}

static int gnss_init_and_start(void)
{
#if defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE) || defined(CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND)
	/* Enable GNSS. */
	if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS) != 0)
	{
		LOG_ERR("Failed to activate GNSS functional mode");
		return -1;
	}
#endif /* CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE || CONFIG_GNSS_SAMPLE_LTE_ON_DEMAND */

	/* Configure GNSS. */ 
	if (nrf_modem_gnss_event_handler_set(gnss_event_handler) != 0)
	{
		LOG_ERR("Failed to set GNSS event handler");
		return -1;
	}

	/* Enable all supported NMEA messages. */
	uint16_t nmea_mask = NRF_MODEM_GNSS_NMEA_RMC_MASK |
						 NRF_MODEM_GNSS_NMEA_GGA_MASK |
						 NRF_MODEM_GNSS_NMEA_GLL_MASK |
						 NRF_MODEM_GNSS_NMEA_GSA_MASK |
						 NRF_MODEM_GNSS_NMEA_GSV_MASK;

	if (nrf_modem_gnss_nmea_mask_set(nmea_mask) != 0)
	{
		LOG_ERR("Failed to set GNSS NMEA mask");
		return -1;
	}

	/* This use case flag should always be set. */
	uint8_t use_case = NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START;

	if (IS_ENABLED(CONFIG_GNSS_SAMPLE_MODE_PERIODIC) &&
		!IS_ENABLED(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE))
	{
		/* Disable GNSS scheduled downloads when assistance is used. */
		use_case |= NRF_MODEM_GNSS_USE_CASE_SCHED_DOWNLOAD_DISABLE;
	}

	if (IS_ENABLED(CONFIG_GNSS_SAMPLE_LOW_ACCURACY))
	{
		use_case |= NRF_MODEM_GNSS_USE_CASE_LOW_ACCURACY;
	}

	if (nrf_modem_gnss_use_case_set(use_case) != 0)
	{
		LOG_WRN("Failed to set GNSS use case");
	}

#if defined(CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK)
	if (nrf_modem_gnss_elevation_threshold_set(CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK) != 0)
	{
		LOG_ERR("Failed to set elevation threshold");
		return -1;
	}
	LOG_DBG("Set elevation threshold to %u", CONFIG_NRF_CLOUD_AGPS_ELEVATION_MASK);
#endif

#if defined(CONFIG_GNSS_SAMPLE_MODE_CONTINUOUS)
	/* Default to no power saving. */
	uint8_t power_mode = NRF_MODEM_GNSS_PSM_DISABLED;

#if defined(GNSS_SAMPLE_POWER_SAVING_MODERATE)
	power_mode = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_PERFORMANCE;
#elif defined(GNSS_SAMPLE_POWER_SAVING_HIGH)
	power_mode = NRF_MODEM_GNSS_PSM_DUTY_CYCLING_POWER;
#endif

	// if (nrf_modem_gnss_power_mode_set(power_mode) != 0)
	// {
	// 	LOG_ERR("Failed to set GNSS power saving mode");
	// 	return -1;
	// }
#endif /* CONFIG_GNSS_SAMPLE_MODE_CONTINUOUS */

	/* Default to continuous tracking. */
	uint16_t fix_retry = 0;
	uint16_t fix_interval = 1;

#if defined(CONFIG_GNSS_SAMPLE_MODE_PERIODIC)
	fix_retry = CONFIG_GNSS_SAMPLE_PERIODIC_TIMEOUT;
	fix_interval = CONFIG_GNSS_SAMPLE_PERIODIC_INTERVAL;
#elif defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
	/* Single fix for TTFF test mode. */
	fix_retry = 0;
	fix_interval = 0;
#endif

	if (nrf_modem_gnss_fix_retry_set(fix_retry) != 0)
	{
		LOG_ERR("Failed to set GNSS fix retry");
		return -1;
	}

	if (nrf_modem_gnss_fix_interval_set(fix_interval) != 0)
	{
		LOG_ERR("Failed to set GNSS fix interval");
		return -1;
	}

#if defined(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST)
	k_work_schedule_for_queue(&gnss_work_q, &ttff_test_prepare_work, K_NO_WAIT);
#else /* !CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST */
	if (nrf_modem_gnss_start() != 0)
	{
		LOG_ERR("Failed to start GNSS");
		return -1;
	}
#endif

	return 0;
}

static bool output_paused(void)
{
#if defined(CONFIG_GNSS_SAMPLE_ASSISTANCE_NONE) || defined(CONFIG_GNSS_SAMPLE_LOG_LEVEL_OFF)
	return false;
#else
	return (requesting_assistance || assistance_is_active()) ? true : false;
#endif
}

static void print_satellite_stats(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	uint8_t tracked = 0;
	uint8_t in_fix = 0;
	uint8_t unhealthy = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; ++i)
	{
		if (pvt_data->sv[i].sv > 0)
		{
			tracked++;

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_USED_IN_FIX)
			{
				in_fix++;
			}

			if (pvt_data->sv[i].flags & NRF_MODEM_GNSS_SV_FLAG_UNHEALTHY)
			{
				unhealthy++;
			}
		}
	}

	printf("Tracking: %2d Using: %2d Unhealthy: %d\n", tracked, in_fix, unhealthy);
}

static void print_fix_data(struct nrf_modem_gnss_pvt_data_frame *pvt_data)
{
	printf("Latitude:       %.06f\n", pvt_data->latitude);
	printf("Longitude:      %.06f\n", pvt_data->longitude);
	// printf("Altitude:       %.01f m\n", pvt_data->altitude);
	// printf("Accuracy:       %.01f m\n", pvt_data->accuracy);
	// printf("Speed:          %.01f m/s\n", pvt_data->speed);
	// printf("Speed accuracy: %.01f m/s\n", pvt_data->speed_accuracy);
	// printf("Heading:        %.01f deg\n", pvt_data->heading);
	// printf("Date:           %04u-%02u-%02u\n",
	// 	   pvt_data->datetime.year,
	// 	   pvt_data->datetime.month,
	// 	   pvt_data->datetime.day);
	// printf("Time (UTC):     %02u:%02u:%02u.%03u\n",
	// 	   pvt_data->datetime.hour,
	// 	   pvt_data->datetime.minute,
	// 	   pvt_data->datetime.seconds,
	// 	   pvt_data->datetime.ms);
	// printf("PDOP:           %.01f\n", pvt_data->pdop);
	// printf("HDOP:           %.01f\n", pvt_data->hdop);
	// printf("VDOP:           %.01f\n", pvt_data->vdop);
	// printf("TDOP:           %.01f\n", pvt_data->tdop);
}

//------------ main

void main(void)
{
	int err;
	uint32_t connect_attempt = 0;
	LOG_INF("The MQTT simple sample started");

	printf("Stopping lte\n");
	k_sem_take(&lte_sem, K_FOREVER);


#if defined(CONFIG_MQTT_LIB_TLS)
	err = certificates_provision();
	if (err != 0)
	{
		LOG_ERR("Failed to provision certificates");
		return;
	}
#endif /* defined(CONFIG_MQTT_LIB_TLS) */

	do
	{
		err = modem_configure();
		if (err)
		{
			LOG_INF("Retrying in %d seconds",
					CONFIG_LTE_CONNECT_RETRY_DELAY_S);
			k_sleep(K_SECONDS(CONFIG_LTE_CONNECT_RETRY_DELAY_S));
		}
	} while (err);

	err = client_init(&client);
	if (err != 0)
	{
		LOG_ERR("client_init: %d", err);
		return;
	}

#if defined(CONFIG_DK_LIBRARY)
	dk_buttons_init(button_handler);
#endif

do_connect:
	if (connect_attempt++ > 0)
	{
		LOG_INF("Reconnecting in %d seconds...",
				CONFIG_MQTT_RECONNECT_DELAY_S);
		k_sleep(K_SECONDS(CONFIG_MQTT_RECONNECT_DELAY_S));
	}
	err = mqtt_connect(&client);
	if (err != 0)
	{
		LOG_ERR("mqtt_connect %d", err);
		goto do_connect;
	}

	err = fds_init(&client);
	if (err != 0)
	{
		LOG_ERR("fds_init: %d", err);
		return;
	}

	while (1)
	{
		char at_buf[100];
		err = nrf_modem_at_cmd(at_buf, sizeof(at_buf), "AT+CEDRXS?");
		// if (err)
		// {
		// 	LOG_ERR("Failed to obtain IMEI, error: %d", err);
		// }


		// printf("%s",at_buf);

		//printf("DATA : long: %.2f ---- lat : %.2f ------------", last_longitude, last_latitude);

		err = poll(&fds, 1, mqtt_keepalive_time_left(&client));
		if (err < 0)
		{
			LOG_ERR("poll: %d", errno);
			break;
		}

		err = mqtt_live(&client);
		if ((err != 0) && (err != -EAGAIN))
		{
			LOG_ERR("ERROR: mqtt_live: %d", err);
			break;
		}

		if ((fds.revents & POLLIN) == POLLIN)
		{
			err = mqtt_input(&client);
			if (err != 0)
			{
				LOG_ERR("mqtt_input: %d", err);
				break;
			}
		}

		if ((fds.revents & POLLERR) == POLLERR)
		{
			LOG_ERR("POLLERR");
			break;
		}

		if ((fds.revents & POLLNVAL) == POLLNVAL)
		{
			LOG_ERR("POLLNVAL");
			break;
		}

	}

	LOG_INF("Disconnecting MQTT client...");

	err = mqtt_disconnect(&client);
	if (err)
	{
		LOG_ERR("Could not disconnect MQTT client: %d", err);
	}
	goto do_connect;
}

void gnss() {

	uint8_t cnt = 0;
	struct nrf_modem_gnss_nmea_data_frame *nmea_data;

	LOG_INF("Starting GNSS sample");

	/* Initialize reference coordinates (if used). */
	if (sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LATITUDE) > 1 &&
		sizeof(CONFIG_GNSS_SAMPLE_REFERENCE_LONGITUDE) > 1)
	{
		ref_used = true;
		ref_latitude = atof(CONFIG_GNSS_SAMPLE_REFERENCE_LATITUDE);
		ref_longitude = atof(CONFIG_GNSS_SAMPLE_REFERENCE_LONGITUDE);
	}

	// if (modem_init() != 0)
	// {
	// 	LOG_ERR("Failed to initialize modem");
	// 	return -1;
	// }

	if (sample_init() != 0) //TODO : Remove this
	{
		LOG_ERR("Failed to initialize sample");
		return -1;
	}

	if (gnss_init_and_start() != 0)
	{
		LOG_ERR("Failed to initialize and start GNSS");
		return -1;
	}

	fix_timestamp = k_uptime_get();

	// printf("Disabling lte");
	// if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE) != 0)
	// {
	// 	LOG_ERR("Failed to decativate LTE");
	// 	return;
	// }

	for (;;)
	{
		(void)k_poll(events, 2, K_FOREVER);

		if (events[0].state == K_POLL_STATE_SEM_AVAILABLE &&
			k_sem_take(events[0].sem, K_NO_WAIT) == 0)
		{
			/* New PVT data available */

			if (IS_ENABLED(CONFIG_GNSS_SAMPLE_MODE_TTFF_TEST))
			{
				/* TTFF test mode. */

				/* Calculate the time GNSS has been blocked by LTE. */
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED)
				{
					time_blocked++;
				}
			}
			else if (IS_ENABLED(CONFIG_GNSS_SAMPLE_NMEA_ONLY))
			{
				/* NMEA-only output mode. */

				if (output_paused())
				{
					goto handle_nmea;
				}

				// if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
				// {
				// 	print_distance_from_reference(&last_pvt);
				// }
			}
			else
			{
				/* PVT and NMEA output mode. */

				if (output_paused())
				{
					goto handle_nmea;
				}

				printf("\033[1;1H");
				printf("\033[2J");
				print_satellite_stats(&last_pvt);

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_DEADLINE_MISSED)
				{
					printf("GNSS operation blocked by LTE\n");
				}
				if (last_pvt.flags &
					NRF_MODEM_GNSS_PVT_FLAG_NOT_ENOUGH_WINDOW_TIME)
				{
					printf("Insufficient GNSS time windows\n");
				}
				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_SLEEP_BETWEEN_PVT)
				{
					printf("Sleep period(s) between PVT notifications\n");
				}
				printf("-----------------------------------\n");

				if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID)
				{
					fix_timestamp = k_uptime_get();
					print_fix_data(&last_pvt);
					last_longitude = last_pvt.longitude;
					last_latitude = last_pvt.latitude;

					// enable lte

					printf("Enabling lte");

					if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_NORMAL) != 0)
					{
						LOG_ERR("Failed to activate LTE");
						return;
					}

					printf("Releasing lte \n");
					k_sem_give(&lte_sem);
					
					printf("Stopping gps after getting fix\n");
					k_sem_take(&gps_sem, K_FOREVER);

					printf("Deactivating lte \n");

					if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE) != 0)
					{
						LOG_ERR("Failed to decativate LTE and enable GNSS functional mode");
						return;
					}
				}
				else
				{
					printf("Seconds since last fix: %d\n",
						   (uint32_t)((k_uptime_get() - fix_timestamp) / 1000));
					cnt++;
					printf("Searching [%c]\n", update_indicator[cnt % 4]);
				}

				printf("\nNMEA strings:\n\n");
			}
		}

	handle_nmea:
		if (events[1].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE &&
			k_msgq_get(events[1].msgq, &nmea_data, K_NO_WAIT) == 0)
		{
			/* New NMEA data available */

			if (!output_paused())
			{
				printf("%s", nmea_data->nmea_str);
			}
			k_free(nmea_data);
		}

		events[0].state = K_POLL_STATE_NOT_READY;
		events[1].state = K_POLL_STATE_NOT_READY;
	}

	return 0;
}

K_THREAD_DEFINE(gps_thread_id, 5000, gnss,
				NULL, NULL, NULL,
				11, 0, 0);
