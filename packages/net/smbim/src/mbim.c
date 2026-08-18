#include "mbim.h"

#include <arpa/inet.h>
#include <gio/gio.h>
#include <libmbim-glib.h>
#include <string.h>

#define COMMAND_TIMEOUT_SECONDS 1
#define MAX_QUEUED_EVENTS 64

typedef struct {
	GMainLoop *loop;
	MbimMessage *response;
	MbimDevice *device;
	GError *error;
	gboolean ok;
} AsyncWait;

typedef struct {
	int type;
	char *detail;
} QueuedEvent;

struct SmbimClient {
	MbimDevice *device;
	GQueue *events;
	unsigned int retries;
};

static void set_error(char **out, const char *prefix, GError *error)
{
	if (out) {
		g_free(*out);
		*out = g_strdup_printf("%s: %s", prefix, error ? error->message : "unknown error");
	}
}

static gboolean error_is_timeout(const GError *error)
{
	return error &&
		(g_error_matches(error, MBIM_CORE_ERROR, MBIM_CORE_ERROR_TIMEOUT) ||
		 g_error_matches(error, MBIM_PROTOCOL_ERROR, MBIM_PROTOCOL_ERROR_TIMEOUT_FRAGMENT));
}

static void command_ready(MbimDevice *device, GAsyncResult *result, AsyncWait *wait)
{
	wait->response = mbim_device_command_finish(device, result, &wait->error);
	g_main_loop_quit(wait->loop);
}

static MbimMessage *command_once(SmbimClient *client, MbimMessage *request,
				 const char *name, unsigned int attempt, char **error)
{
	AsyncWait wait = {0};

	wait.loop = g_main_loop_new(NULL, FALSE);
	mbim_device_command(client->device, request, COMMAND_TIMEOUT_SECONDS, NULL,
			    (GAsyncReadyCallback)command_ready, &wait);
	g_main_loop_run(wait.loop);
	g_main_loop_unref(wait.loop);
	if (!wait.response) {
		if (error_is_timeout(wait.error))
			g_printerr("smbim: MBIM command %s timed out after 1 second (attempt %u/%u)\n",
				   name, attempt, client->retries);
		set_error(error, name, wait.error);
		g_clear_error(&wait.error);
		return NULL;
	}
	if (!mbim_message_command_done_get_result(wait.response, &wait.error)) {
		set_error(error, name, wait.error);
		g_clear_error(&wait.error);
		mbim_message_unref(wait.response);
		return NULL;
	}
	if (error) {
		g_free(*error);
		*error = NULL;
	}
	return wait.response;
}

static void device_new_ready(GObject *source, GAsyncResult *result, AsyncWait *wait)
{
	(void)source;
	wait->device = mbim_device_new_finish(result, &wait->error);
	g_main_loop_quit(wait->loop);
}

static void device_open_ready(MbimDevice *device, GAsyncResult *result, AsyncWait *wait)
{
	wait->ok = mbim_device_open_full_finish(device, result, &wait->error);
	g_main_loop_quit(wait->loop);
}

static void queue_event(SmbimClient *client, int type, const char *detail)
{
	GList *cursor;

	/* All callbacks and API calls run on the same GLib main context. */
	for (cursor = client->events->head; cursor; cursor = cursor->next) {
		QueuedEvent *queued = cursor->data;
		if (queued->type == type)
			return;
	}
	if (g_queue_get_length(client->events) >= MAX_QUEUED_EVENTS) {
		QueuedEvent *oldest = g_queue_pop_head(client->events);
		g_free(oldest->detail);
		g_free(oldest);
	}
	QueuedEvent *event = g_new0(QueuedEvent, 1);
	event->type = type;
	event->detail = g_strdup(detail ? detail : "");
	g_queue_push_tail(client->events, event);
}

static void indicate_status(MbimDevice *device, MbimMessage *message, SmbimClient *client)
{
	guint32 cid;
	(void)device;
	if (mbim_message_indicate_status_get_service(message) != MBIM_SERVICE_BASIC_CONNECT)
		return;
	cid = mbim_message_indicate_status_get_cid(message);
	if (cid == MBIM_CID_BASIC_CONNECT_IP_CONFIGURATION)
		queue_event(client, SMBIM_EVENT_IP_CONFIGURATION, "IP configuration indication received");
	else if (cid == MBIM_CID_BASIC_CONNECT_CONNECT)
		queue_event(client, SMBIM_EVENT_CONNECT, "Connect state indication received");
	else if (cid == MBIM_CID_BASIC_CONNECT_PACKET_SERVICE)
		queue_event(client, SMBIM_EVENT_PACKET_SERVICE, "Packet service indication received");
	else if (cid == MBIM_CID_BASIC_CONNECT_REGISTER_STATE)
		queue_event(client, SMBIM_EVENT_REGISTER_STATE, "Register state indication received");
}

static void device_error(MbimDevice *device, GError *error, SmbimClient *client)
{
	(void)device;
	queue_event(client, SMBIM_EVENT_DEVICE_ERROR, error ? error->message : "MBIM device error");
}

static void device_removed(MbimDevice *device, SmbimClient *client)
{
	(void)device;
	queue_event(client, SMBIM_EVENT_DEVICE_REMOVED, "MBIM device was removed");
}

SmbimClient *smbim_client_open(const char *path, int use_proxy, unsigned int retries, char **error)
{
	SmbimClient *client = g_new0(SmbimClient, 1);
	GFile *file = g_file_new_for_path(path);
	AsyncWait wait = {0};
	MbimDeviceOpenFlags flags = use_proxy ? MBIM_DEVICE_OPEN_FLAGS_PROXY : MBIM_DEVICE_OPEN_FLAGS_NONE;

	client->events = g_queue_new();
	client->retries = retries ? retries : 1;
	wait.loop = g_main_loop_new(NULL, FALSE);
	mbim_device_new(file, NULL, (GAsyncReadyCallback)device_new_ready, &wait);
	g_main_loop_run(wait.loop);
	g_main_loop_unref(wait.loop);
	g_object_unref(file);
	if (!wait.device) {
		set_error(error, "create MBIM device", wait.error);
		g_clear_error(&wait.error);
		smbim_client_close(client);
		return NULL;
	}
	client->device = wait.device;
	wait = (AsyncWait){0};
	wait.loop = g_main_loop_new(NULL, FALSE);
	mbim_device_open_full(client->device, flags, COMMAND_TIMEOUT_SECONDS, NULL,
			      (GAsyncReadyCallback)device_open_ready, &wait);
	g_main_loop_run(wait.loop);
	g_main_loop_unref(wait.loop);
	if (!wait.ok) {
		set_error(error, "open MBIM device", wait.error);
		g_clear_error(&wait.error);
		smbim_client_close(client);
		return NULL;
	}
	g_signal_connect(client->device, MBIM_DEVICE_SIGNAL_INDICATE_STATUS, G_CALLBACK(indicate_status), client);
	g_signal_connect(client->device, MBIM_DEVICE_SIGNAL_ERROR, G_CALLBACK(device_error), client);
	g_signal_connect(client->device, MBIM_DEVICE_SIGNAL_REMOVED, G_CALLBACK(device_removed), client);
	return client;
}

void smbim_client_close(SmbimClient *client)
{
	if (!client)
		return;
	if (client->device) {
		g_signal_handlers_disconnect_by_data(client->device, client);
		if (mbim_device_is_open(client->device))
			mbim_device_close_force(client->device, NULL);
		g_object_unref(client->device);
	}
	if (client->events) {
		while (!g_queue_is_empty(client->events)) {
			QueuedEvent *event = g_queue_pop_head(client->events);
			g_free(event->detail);
			g_free(event);
		}
		g_queue_free(client->events);
	}
	g_free(client);
}

int smbim_subscribe(SmbimClient *client, char **error)
{
	guint32 cids[] = { MBIM_CID_BASIC_CONNECT_REGISTER_STATE, MBIM_CID_BASIC_CONNECT_PACKET_SERVICE,
		MBIM_CID_BASIC_CONNECT_CONNECT, MBIM_CID_BASIC_CONNECT_IP_CONFIGURATION };
	MbimEventEntry entry = {0};
	const MbimEventEntry *entries[] = { &entry, NULL };
	unsigned int attempt;

	entry.device_service_id = *MBIM_UUID_BASIC_CONNECT;
	entry.cids_count = G_N_ELEMENTS(cids);
	entry.cids = cids;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_device_service_subscribe_list_set_new(1, entries, NULL);
		MbimMessage *response = command_once(client, request, "subscribe indications", attempt, error);
		mbim_message_unref(request);
		if (response) {
			mbim_message_unref(response);
			return 1;
		}
	}
	return 0;
}

int smbim_pin_status(SmbimClient *client, int *pin_type, int *pin_state, char **error)
{
	unsigned int attempt;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_pin_query_new(NULL);
		MbimMessage *response = command_once(client, request, "query PIN state", attempt, error);
		MbimPinType type;
		MbimPinState state;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (mbim_message_pin_response_parse(response, &type, &state, NULL, NULL)) {
			*pin_type = type;
			*pin_state = state;
			mbim_message_unref(response);
			return 1;
		}
		mbim_message_unref(response);
	}
	return 0;
}

int smbim_unlock_pin_once(SmbimClient *client, const char *pin, unsigned int attempt, char **error)
{
	MbimMessage *request = mbim_message_pin_set_new(MBIM_PIN_TYPE_PIN1, MBIM_PIN_OPERATION_ENTER, pin, NULL, NULL);
	MbimMessage *response = command_once(client, request, "unlock PIN", attempt, error);
	mbim_message_unref(request);
	if (!response)
		return 0;
	mbim_message_unref(response);
	return 1;
}

int smbim_subscriber_status(SmbimClient *client, int *ready_state, char **error)
{
	unsigned int attempt;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_subscriber_ready_status_query_new(NULL);
		MbimMessage *response = command_once(client, request, "query subscriber state", attempt, error);
		MbimSubscriberReadyState state;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (mbim_message_subscriber_ready_status_response_parse(response, &state, NULL, NULL, NULL, NULL, NULL, NULL)) {
			*ready_state = state;
			mbim_message_unref(response);
			return 1;
		}
		mbim_message_unref(response);
	}
	return 0;
}

int smbim_register_status(SmbimClient *client, int *register_state, char **error)
{
	unsigned int attempt;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_register_state_query_new(NULL);
		MbimMessage *response = command_once(client, request, "query register state", attempt, error);
		MbimRegisterState state;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (mbim_message_register_state_response_parse(response, NULL, &state, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) {
			*register_state = state;
			mbim_message_unref(response);
			return 1;
		}
		mbim_message_unref(response);
	}
	return 0;
}

int smbim_packet_status(SmbimClient *client, int *packet_state, char **error)
{
	unsigned int attempt;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_packet_service_query_new(NULL);
		MbimMessage *response = command_once(client, request, "query packet service", attempt, error);
		MbimPacketServiceState state;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (mbim_message_packet_service_response_parse(response, NULL, &state, NULL, NULL, NULL, NULL)) {
			*packet_state = state;
			mbim_message_unref(response);
			return 1;
		}
		mbim_message_unref(response);
	}
	return 0;
}

int smbim_attach_once(SmbimClient *client, unsigned int attempt, char **error)
{
	MbimMessage *request = mbim_message_packet_service_set_new(MBIM_PACKET_SERVICE_ACTION_ATTACH, NULL);
	MbimMessage *response = command_once(client, request, "attach packet service", attempt, error);
	mbim_message_unref(request);
	if (!response)
		return 0;
	mbim_message_unref(response);
	return 1;
}

int smbim_connect_status(SmbimClient *client, int *activation_state, int *ip_type, char **error)
{
	unsigned int attempt;
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_connect_query_new(0, MBIM_ACTIVATION_STATE_UNKNOWN,
			MBIM_VOICE_CALL_STATE_NONE, MBIM_CONTEXT_IP_TYPE_DEFAULT,
			mbim_uuid_from_context_type(MBIM_CONTEXT_TYPE_INTERNET), 0, NULL);
		MbimMessage *response = command_once(client, request, "query connect state", attempt, error);
		MbimActivationState state;
		MbimContextIpType type;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (mbim_message_connect_response_parse(response, NULL, &state, NULL, &type, NULL, NULL, NULL)) {
			*activation_state = state;
			*ip_type = type;
			mbim_message_unref(response);
			return 1;
		}
		mbim_message_unref(response);
	}
	return 0;
}

int smbim_connect_once(SmbimClient *client, const char *apn, const char *username,
		       const char *password, int auth, int ip_type, unsigned int attempt, char **error)
{
	MbimMessage *request = mbim_message_connect_set_new(0, MBIM_ACTIVATION_COMMAND_ACTIVATE, apn,
		username, password, MBIM_COMPRESSION_NONE, (MbimAuthProtocol)auth,
		(MbimContextIpType)ip_type, mbim_uuid_from_context_type(MBIM_CONTEXT_TYPE_INTERNET), NULL);
	MbimMessage *response = command_once(client, request, "activate connection", attempt, error);
	mbim_message_unref(request);
	if (!response)
		return 0;
	mbim_message_unref(response);
	return 1;
}

int smbim_disconnect_once(SmbimClient *client, unsigned int attempt, char **error)
{
	MbimMessage *request = mbim_message_connect_set_new(0, MBIM_ACTIVATION_COMMAND_DEACTIVATE, NULL,
		NULL, NULL, MBIM_COMPRESSION_NONE, MBIM_AUTH_PROTOCOL_NONE, MBIM_CONTEXT_IP_TYPE_DEFAULT,
		mbim_uuid_from_context_type(MBIM_CONTEXT_TYPE_INTERNET), NULL);
	MbimMessage *response = command_once(client, request, "deactivate connection", attempt, error);
	mbim_message_unref(request);
	if (!response)
		return 0;
	mbim_message_unref(response);
	return 1;
}

static gint compare_strings(gconstpointer left, gconstpointer right)
{
	return g_strcmp0(*(char * const *)left, *(char * const *)right);
}

static char *format_address(const guint8 *address, int family, guint32 prefix,
			    gboolean include_prefix, gboolean mask_host)
{
	guint8 normalized[sizeof(struct in6_addr)] = {0};
	gsize length = family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
	char buffer[INET6_ADDRSTRLEN];
	guint32 bit;

	if (!address || (family != AF_INET && family != AF_INET6) || prefix > length * 8)
		return NULL;
	for (bit = 0; bit < length; bit++)
		normalized[bit] = address[bit];
	if (mask_host) {
		for (bit = prefix; bit < length * 8; bit++)
			normalized[bit / 8] &= (guint8)~(1U << (7 - (bit % 8)));
	}
	if (!inet_ntop(family, normalized, buffer, sizeof(buffer)))
		return NULL;
	return include_prefix ? g_strdup_printf("%s/%u", buffer, prefix) : g_strdup(buffer);
}

static void append_json_array(GString *output, GPtrArray *values)
{
	guint i;

	for (i = 0; i < values->len; i++) {
		if (i)
			g_string_append_c(output, ',');
		g_string_append_printf(output, "\"%s\"", (char *)g_ptr_array_index(values, i));
	}
}

static void append_fingerprint_array(GString *output, GPtrArray *values)
{
	guint i;

	g_ptr_array_sort(values, compare_strings);
	for (i = 0; i < values->len; i++) {
		const char *value = g_ptr_array_index(values, i);
		g_string_append_printf(output, "%zu:%s,", strlen(value), value);
	}
}

int smbim_query_ip_configuration(SmbimClient *client, SmbimIpConfiguration *config, char **error)
{
	unsigned int attempt;

	if (!config) {
		if (error) {
			g_free(*error);
			*error = g_strdup("query IP configuration: missing output storage");
		}
		return 0;
	}
	smbim_ip_configuration_clear(config);
	for (attempt = 1; attempt <= client->retries; attempt++) {
		MbimMessage *request = mbim_message_ip_configuration_query_new(0, 0, 0, 0, NULL, 0, NULL,
			NULL, NULL, 0, NULL, 0, NULL, 0, 0, NULL);
		MbimMessage *response = command_once(client, request, "query IP configuration", attempt, error);
		guint32 session, v4_count, v6_count, v4_dns_count, v6_dns_count, v4_mtu, v6_mtu, i;
		MbimIPConfigurationAvailableFlag v4_available, v6_available;
		MbimIPv4ElementArray *v4 = NULL;
		MbimIPv6ElementArray *v6 = NULL;
		const MbimIPv4 *v4_gateway = NULL;
		const MbimIPv6 *v6_gateway = NULL;
		MbimIPv4 *v4_dns = NULL;
		MbimIPv6 *v6_dns = NULL;
		GString *json, *fingerprint;
		GPtrArray *v4_addresses, *v6_addresses, *v6_networks, *v4_dns_values, *v6_dns_values;
		char *v4_gateway_value = NULL, *v6_gateway_value = NULL;
		gboolean has_addresses, has_ipv4, has_ipv6;
		mbim_message_unref(request);
		if (!response)
			continue;
		if (!mbim_message_ip_configuration_response_parse(response, &session, &v4_available, &v6_available,
			&v4_count, &v4, &v6_count, &v6, &v4_gateway, &v6_gateway, &v4_dns_count, &v4_dns,
			&v6_dns_count, &v6_dns, &v4_mtu, &v6_mtu, NULL)) {
			set_error(error, "parse IP configuration", NULL);
			mbim_message_unref(response);
			continue;
		}
		v4_addresses = g_ptr_array_new_with_free_func(g_free);
		v6_addresses = g_ptr_array_new_with_free_func(g_free);
		v6_networks = g_ptr_array_new_with_free_func(g_free);
		v4_dns_values = g_ptr_array_new_with_free_func(g_free);
		v6_dns_values = g_ptr_array_new_with_free_func(g_free);
		for (i = 0; i < v4_count; i++) {
			char *value = format_address(v4[i]->ipv4_address.addr, AF_INET,
				v4[i]->on_link_prefix_length, TRUE, FALSE);
			if (value)
				g_ptr_array_add(v4_addresses, value);
		}
		for (i = 0; i < v6_count; i++) {
			char *address_value = format_address(v6[i]->ipv6_address.addr, AF_INET6,
				v6[i]->on_link_prefix_length, TRUE, FALSE);
			char *network_value = format_address(v6[i]->ipv6_address.addr, AF_INET6,
				v6[i]->on_link_prefix_length, TRUE, TRUE);
			if (address_value)
				g_ptr_array_add(v6_addresses, address_value);
			if (network_value)
				g_ptr_array_add(v6_networks, network_value);
		}
		for (i = 0; i < v4_dns_count; i++) {
			char *value = format_address(v4_dns[i].addr, AF_INET, 32, FALSE, FALSE);
			if (value)
				g_ptr_array_add(v4_dns_values, value);
		}
		for (i = 0; i < v6_dns_count; i++) {
			char *value = format_address(v6_dns[i].addr, AF_INET6, 128, FALSE, FALSE);
			if (value)
				g_ptr_array_add(v6_dns_values, value);
		}
		if (v4_gateway)
			v4_gateway_value = format_address(v4_gateway->addr, AF_INET, 32, FALSE, FALSE);
		if (v6_gateway)
			v6_gateway_value = format_address(v6_gateway->addr, AF_INET6, 128, FALSE, FALSE);
		json = g_string_new(NULL);
		g_string_append_printf(json, "{\"session_id\":%u,\"ipv4\":{\"available\":%u,\"addresses\":[", session, v4_available);
		append_json_array(json, v4_addresses);
		g_string_append(json, "],\"gateway\":");
		g_string_append_printf(json, "\"%s\"", v4_gateway_value ? v4_gateway_value : "");
		g_string_append(json, ",\"dns\":[");
		append_json_array(json, v4_dns_values);
		g_string_append_printf(json, "],\"mtu\":%u},\"ipv6\":{\"available\":%u,\"addresses\":[", v4_mtu, v6_available);
		append_json_array(json, v6_addresses);
		g_string_append(json, "],\"gateway\":");
		g_string_append_printf(json, "\"%s\"", v6_gateway_value ? v6_gateway_value : "");
		g_string_append(json, ",\"dns\":[");
		append_json_array(json, v6_dns_values);
		g_string_append_printf(json, "],\"mtu\":%u}}", v6_mtu);
		fingerprint = g_string_new(NULL);
		g_string_append_printf(fingerprint, "s=%u|v4a=", session);
		append_fingerprint_array(fingerprint, v4_addresses);
		g_string_append_printf(fingerprint, "|v4g=%s|v4d=", v4_gateway_value ? v4_gateway_value : "");
		append_fingerprint_array(fingerprint, v4_dns_values);
		g_string_append_printf(fingerprint, "|v4m=%u|v6n=", v4_mtu);
		append_fingerprint_array(fingerprint, v6_networks);
		g_string_append(fingerprint, "|v6d=");
		append_fingerprint_array(fingerprint, v6_dns_values);
		g_string_append_printf(fingerprint, "|v6m=%u", v6_mtu);
		has_ipv4 = v4_addresses->len > 0;
		has_ipv6 = v6_addresses->len > 0;
		has_addresses = has_ipv4 || has_ipv6;
		mbim_ipv4_element_array_free(v4);
		mbim_ipv6_element_array_free(v6);
		g_free(v4_dns);
		g_free(v6_dns);
		g_ptr_array_free(v4_addresses, TRUE);
		g_ptr_array_free(v6_addresses, TRUE);
		g_ptr_array_free(v6_networks, TRUE);
		g_ptr_array_free(v4_dns_values, TRUE);
		g_ptr_array_free(v6_dns_values, TRUE);
		g_free(v4_gateway_value);
		g_free(v6_gateway_value);
		mbim_message_unref(response);
		config->json = g_string_free(json, FALSE);
		config->fingerprint = g_string_free(fingerprint, FALSE);
		config->has_addresses = has_addresses;
		config->has_ipv4 = has_ipv4;
		config->has_ipv6 = has_ipv6;
		return 1;
	}
	return 0;
}

void smbim_ip_configuration_clear(SmbimIpConfiguration *config)
{
	if (!config)
		return;
	g_clear_pointer(&config->json, g_free);
	g_clear_pointer(&config->fingerprint, g_free);
	config->has_addresses = 0;
	config->has_ipv4 = 0;
	config->has_ipv6 = 0;
}

static gboolean poll_timeout(gpointer data)
{
	*((gboolean *)data) = TRUE;
	return G_SOURCE_REMOVE;
}

int smbim_poll_event(SmbimClient *client, unsigned int timeout_ms, char **detail)
{
	gboolean expired = FALSE;
	GSource *timer = NULL;
	QueuedEvent *event;
	int type;

	if (g_queue_is_empty(client->events)) {
		timer = g_timeout_source_new(timeout_ms);
		g_source_set_callback(timer, poll_timeout, &expired, NULL);
		g_source_attach(timer, NULL);
		while (!expired && g_queue_is_empty(client->events))
			g_main_context_iteration(NULL, TRUE);
		g_source_destroy(timer);
		g_source_unref(timer);
	}
	if (g_queue_is_empty(client->events))
		return SMBIM_EVENT_NONE;
	event = g_queue_pop_head(client->events);
	if (detail)
		*detail = g_strdup(event->detail);
	type = event->type;
	g_free(event->detail);
	g_free(event);
	return type;
}

void smbim_string_free(char *value)
{
	g_free(value);
}
