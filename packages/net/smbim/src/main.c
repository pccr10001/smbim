#include "mbim.h"

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <gio/gio.h>
#include <libmbim-glib.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define DEVICE_WAIT_SECONDS 60
#define SUBSCRIBER_WAIT_SECONDS 60
#define REGISTRATION_WAIT_SECONDS 120

typedef struct {
	char *interface;
	char *device;
	char *ifname;
	char *apn;
	char *pin;
	char *auth;
	char *username;
	char *password;
	char *ip_type;
	char *update_helper;
	char *custom_dns;
	gboolean proxy;
	gboolean allow_roaming;
	gboolean allow_partner;
	gboolean default_route;
	gboolean peer_dns;
	gboolean delegate;
	gboolean source_filter;
	gint retries;
	gint poll_interval;
	gint metric;
	gint mtu;
} Config;

typedef struct {
	Config config;
	SmbimClient *client;
	char *last_fingerprint;
	gboolean has_ip;
} Daemon;

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t reconnect_requested;

static void handle_shutdown_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void handle_reconnect_signal(int signo)
{
	(void)signo;
	reconnect_requested = 1;
}

static void log_message(const char *format, ...)
{
	va_list arguments;
	char *message;
	unsigned char *cursor;

	va_start(arguments, format);
	message = g_strdup_vprintf(format, arguments);
	va_end(arguments);
	/* Keep modem- and helper-controlled text from forging extra log lines. */
	for (cursor = (unsigned char *)message; *cursor; cursor++) {
		if (*cursor < 0x20 || *cursor == 0x7f)
			*cursor = ' ';
	}
	fputs("smbim: ", stderr);
	fputs(message, stderr);
	fputc('\n', stderr);
	g_free(message);
}

static gboolean fail_with(char **error, const char *format, ...)
{
	va_list arguments;

	if (error) {
		g_free(*error);
		va_start(arguments, format);
		*error = g_strdup_vprintf(format, arguments);
		va_end(arguments);
	}
	return FALSE;
}

static void clear_error(char **error)
{
	if (error)
		g_clear_pointer(error, g_free);
}

static gboolean wait_milliseconds(guint milliseconds)
{
	guint elapsed = 0;

	while (!stop_requested && elapsed < milliseconds) {
		guint slice = MIN(100U, milliseconds - elapsed);
		g_usleep((gulong)slice * 1000U);
		elapsed += slice;
	}
	return !stop_requested;
}

static gboolean file_exists(const char *path)
{
	return path && g_file_test(path, G_FILE_TEST_EXISTS);
}

static char *find_network_interface(const char *device, const char *configured)
{
	const char *classes[] = { "usbmisc", "wwan" };
	char *basename = NULL;
	char *candidate = NULL;
	guint i;

	if (configured && *configured) {
		char *safe_name = g_path_get_basename(configured);
		char *path = g_build_filename("/sys/class/net", safe_name, NULL);
		if (file_exists(path))
			candidate = g_strdup(safe_name);
		g_free(path);
		g_free(safe_name);
		if (candidate)
			return candidate;
	}
	basename = g_path_get_basename(device);
	for (i = 0; i < G_N_ELEMENTS(classes) && !candidate; i++) {
		char *directory = g_build_filename("/sys/class", classes[i], basename,
			"device", "net", NULL);
		GError *directory_error = NULL;
		GDir *dir = g_dir_open(directory, 0, &directory_error);
		if (dir) {
			const char *entry;
			while ((entry = g_dir_read_name(dir)) != NULL) {
				char *net_path = g_build_filename("/sys/class/net", entry, NULL);
				if (file_exists(net_path)) {
					candidate = g_strdup(entry);
					g_free(net_path);
					break;
				}
				g_free(net_path);
			}
			g_dir_close(dir);
		}
		g_clear_error(&directory_error);
		g_free(directory);
	}
	g_free(basename);
	return candidate;
}

static gboolean device_ready(Config *config)
{
	struct stat status;
	char *ifname;

	if (stat(config->device, &status) != 0 || !S_ISCHR(status.st_mode))
		return FALSE;
	ifname = find_network_interface(config->device, config->ifname);
	if (!ifname)
		return FALSE;
	g_free(config->ifname);
	config->ifname = ifname;
	return TRUE;
}

static gboolean wait_for_device(Config *config, char **error)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)DEVICE_WAIT_SECONDS * G_USEC_PER_SEC;
	gint64 next_log = 0;

	while (!stop_requested) {
		gint64 now = g_get_monotonic_time();
		if (device_ready(config))
			return TRUE;
		if (now >= deadline)
			return fail_with(error, "MBIM device %s is not present after 60 seconds", config->device);
		if (now >= next_log) {
			log_message("Waiting for MBIM device %s and its network interface", config->device);
			next_log = now + (gint64)30 * G_USEC_PER_SEC;
		}
		wait_milliseconds(1000);
	}
	return fail_with(error, "shutdown requested while waiting for MBIM device");
}

static gboolean open_with_retry(Daemon *daemon, char **error)
{
	gint attempt;
	char *last_error = NULL;

	for (attempt = 1; attempt <= daemon->config.retries && !stop_requested; attempt++) {
		daemon->client = smbim_client_open(daemon->config.device, daemon->config.proxy,
			(guint)daemon->config.retries, &last_error);
		if (daemon->client)
			return TRUE;
		log_message("Open attempt %d/%d failed: %s", attempt, daemon->config.retries,
			last_error ? last_error : "unknown error");
		wait_milliseconds(200);
	}
	if (last_error) {
		fail_with(error, "%s", last_error);
		g_free(last_error);
		return FALSE;
	}
	return fail_with(error, "shutdown requested while opening MBIM device");
}

static gboolean wait_for_subscriber(Daemon *daemon, char **error)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)SUBSCRIBER_WAIT_SECONDS * G_USEC_PER_SEC;

	while (!stop_requested) {
		int state = 0;
		char *query_error = NULL;
		if (smbim_subscriber_status(daemon->client, &state, &query_error) &&
		    state == MBIM_SUBSCRIBER_READY_STATE_INITIALIZED) {
			clear_error(&query_error);
			log_message("Subscriber is initialized");
			return TRUE;
		}
		clear_error(&query_error);
		if (g_get_monotonic_time() >= deadline)
			return fail_with(error, "subscriber did not initialize within 60 seconds");
		wait_milliseconds(1000);
	}
	return fail_with(error, "shutdown requested while waiting for subscriber");
}

static gboolean prepare_sim(Daemon *daemon, char **error)
{
	int pin_type = 0, pin_state = 0;
	char *command_error = NULL;

	if (!smbim_pin_status(daemon->client, &pin_type, &pin_state, &command_error)) {
		fail_with(error, "query PIN state: %s", command_error ? command_error : "unknown error");
		clear_error(&command_error);
		return FALSE;
	}
	if (pin_state == MBIM_PIN_STATE_LOCKED) {
		if (pin_type == MBIM_PIN_TYPE_PIN2) {
			log_message("PIN2 is locked; continuing because data service does not require it");
			return wait_for_subscriber(daemon, error);
		}
		if (pin_type != MBIM_PIN_TYPE_PIN1)
			return fail_with(error, "modem requires unsupported PIN type %d", pin_type);
		if (!daemon->config.pin || !*daemon->config.pin)
			return fail_with(error, "SIM PIN1 is required");
		/* A wrong PIN consumes a limited SIM attempt, so never retry PIN entry blindly. */
		smbim_unlock_pin_once(daemon->client, daemon->config.pin, 1, &command_error);
		clear_error(&command_error);
		if (!smbim_pin_status(daemon->client, &pin_type, &pin_state, &command_error) ||
		    pin_state != MBIM_PIN_STATE_UNLOCKED) {
			fail_with(error, "SIM PIN was not accepted: %s",
				command_error ? command_error : "SIM remains locked");
			clear_error(&command_error);
			return FALSE;
		}
		clear_error(&command_error);
		log_message("SIM PIN accepted");
	}
	return wait_for_subscriber(daemon, error);
}

static gboolean wait_for_registration(Daemon *daemon, char **error)
{
	gint64 deadline = g_get_monotonic_time() + (gint64)REGISTRATION_WAIT_SECONDS * G_USEC_PER_SEC;

	while (!stop_requested) {
		int state = 0;
		char *query_error = NULL;
		if (smbim_register_status(daemon->client, &state, &query_error)) {
			gboolean allowed = state == MBIM_REGISTER_STATE_HOME ||
				(state == MBIM_REGISTER_STATE_ROAMING && daemon->config.allow_roaming) ||
				(state == MBIM_REGISTER_STATE_PARTNER && daemon->config.allow_partner);
			if (allowed) {
				clear_error(&query_error);
				log_message("Registered with the cellular network (state=%d)", state);
				return TRUE;
			}
		}
		clear_error(&query_error);
		if (g_get_monotonic_time() >= deadline)
			return fail_with(error, "network registration timed out after 120 seconds");
		wait_milliseconds(1000);
	}
	return fail_with(error, "shutdown requested while waiting for network registration");
}

static gboolean ensure_attached(Daemon *daemon, char **error)
{
	int state = 0;
	char *command_error = NULL;
	gint attempt;

	if (smbim_packet_status(daemon->client, &state, &command_error) &&
	    state == MBIM_PACKET_SERVICE_STATE_ATTACHED) {
		clear_error(&command_error);
		return TRUE;
	}
	clear_error(&command_error);
	for (attempt = 1; attempt <= daemon->config.retries && !stop_requested; attempt++) {
		smbim_attach_once(daemon->client, (guint)attempt, &command_error);
		clear_error(&command_error);
		if (smbim_packet_status(daemon->client, &state, &command_error) &&
		    state == MBIM_PACKET_SERVICE_STATE_ATTACHED) {
			clear_error(&command_error);
			log_message("Packet service is attached");
			return TRUE;
		}
		log_message("Attach attempt %d/%d was not confirmed: %s", attempt,
			daemon->config.retries, command_error ? command_error : "unknown error");
		clear_error(&command_error);
		wait_milliseconds(200);
	}
	return fail_with(error, "packet service attach failed after retries");
}

static gboolean ensure_connected(Daemon *daemon, MbimAuthProtocol auth,
				 MbimContextIpType ip_type, char **error)
{
	int state = 0, current_ip_type = 0;
	char *command_error = NULL;
	gint attempt;

	if (smbim_connect_status(daemon->client, &state, &current_ip_type, &command_error) &&
	    state == MBIM_ACTIVATION_STATE_ACTIVATED) {
		clear_error(&command_error);
		return TRUE;
	}
	clear_error(&command_error);
	for (attempt = 1; attempt <= daemon->config.retries && !stop_requested; attempt++) {
		smbim_connect_once(daemon->client, daemon->config.apn, daemon->config.username,
			daemon->config.password, auth, ip_type, (guint)attempt, &command_error);
		clear_error(&command_error);
		if (smbim_connect_status(daemon->client, &state, &current_ip_type, &command_error) &&
		    state == MBIM_ACTIVATION_STATE_ACTIVATED) {
			clear_error(&command_error);
			log_message("Data session is activated");
			return TRUE;
		}
		log_message("Connect attempt %d/%d was not confirmed: %s", attempt,
			daemon->config.retries, command_error ? command_error : "unknown error");
		clear_error(&command_error);
		wait_milliseconds(200);
	}
	return fail_with(error, "data session activation failed after retries");
}

static gboolean ensure_online(Daemon *daemon, MbimAuthProtocol auth,
			      MbimContextIpType ip_type, char **error)
{
	return wait_for_registration(daemon, error) &&
		ensure_attached(daemon, error) &&
		ensure_connected(daemon, auth, ip_type, error);
}

static void disconnect_verified(Daemon *daemon)
{
	gint attempt;

	for (attempt = 1; attempt <= daemon->config.retries; attempt++) {
		int state = 0, ip_type = 0;
		char *command_error = NULL;
		smbim_disconnect_once(daemon->client, (guint)attempt, &command_error);
		clear_error(&command_error);
		if (smbim_connect_status(daemon->client, &state, &ip_type, &command_error) &&
		    state != MBIM_ACTIVATION_STATE_ACTIVATED) {
			clear_error(&command_error);
			log_message("Data session is deactivated");
			return;
		}
		log_message("Disconnect attempt %d/%d was not confirmed: %s", attempt,
			daemon->config.retries, command_error ? command_error : "unknown error");
		clear_error(&command_error);
	}
}

static gboolean run_helper(const Config *config, const char *action, const char *payload,
			   char **error)
{
	GSubprocessLauncher *launcher;
	GSubprocess *process;
	GError *spawn_error = NULL;
	char *metric = g_strdup_printf("%d", config->metric);
	char *mtu = g_strdup_printf("%d", config->mtu);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE);
	/* Credentials are only for MBIM authentication and must not reach helpers. */
	g_subprocess_launcher_unsetenv(launcher, "SMBIM_PIN");
	g_subprocess_launcher_unsetenv(launcher, "SMBIM_USERNAME");
	g_subprocess_launcher_unsetenv(launcher, "SMBIM_PASSWORD");
	g_subprocess_launcher_setenv(launcher, "SMBIM_INTERFACE", config->interface, TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_IFNAME", config->ifname, TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_IP_TYPE", config->ip_type, TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_DEFAULT_ROUTE", config->default_route ? "1" : "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_METRIC", metric, TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_PEER_DNS", config->peer_dns ? "1" : "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_CUSTOM_DNS", config->custom_dns, TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_DELEGATE", config->delegate ? "1" : "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_SOURCE_FILTER", config->source_filter ? "1" : "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "SMBIM_MTU", mtu, TRUE);
	process = g_subprocess_launcher_spawn(launcher, &spawn_error, config->update_helper, action, NULL);
	g_object_unref(launcher);
	g_free(metric);
	g_free(mtu);
	if (!process) {
		fail_with(error, "netifd helper %s: %s", action,
			spawn_error ? spawn_error->message : "spawn failed");
		g_clear_error(&spawn_error);
		return FALSE;
	}
	if (!g_subprocess_communicate_utf8(process, payload ? payload : "", NULL, NULL, NULL,
					 &spawn_error) || !g_subprocess_get_successful(process)) {
		fail_with(error, "netifd helper %s: %s", action,
			spawn_error ? spawn_error->message : "non-zero exit status");
		g_clear_error(&spawn_error);
		g_object_unref(process);
		return FALSE;
	}
	g_object_unref(process);
	return TRUE;
}

static gboolean required_ip_is_present(const Config *config,
				       const SmbimIpConfiguration *current)
{
	if (g_ascii_strcasecmp(config->ip_type, "ipv4") == 0)
		return current->has_ipv4;
	if (g_ascii_strcasecmp(config->ip_type, "ipv6") == 0)
		return current->has_ipv6;
	if (g_ascii_strcasecmp(config->ip_type, "ipv4v6") == 0)
		return current->has_ipv4 && current->has_ipv6;
	return current->has_addresses;
}

static gboolean refresh_ip(Daemon *daemon, gboolean *missing_required_ip, char **error)
{
	SmbimIpConfiguration current = {0};
	char *query_error = NULL;

	if (missing_required_ip)
		*missing_required_ip = FALSE;

	if (!smbim_query_ip_configuration(daemon->client, &current, &query_error)) {
		fail_with(error, "%s", query_error ? query_error : "query IP configuration failed");
		clear_error(&query_error);
		return FALSE;
	}
	if (!required_ip_is_present(&daemon->config, &current)) {
		if (missing_required_ip)
			*missing_required_ip = TRUE;
		smbim_ip_configuration_clear(&current);
		return fail_with(error, "modem returned no address matching PDP type %s",
			daemon->config.ip_type);
	}
	if (daemon->has_ip && g_strcmp0(daemon->last_fingerprint, current.fingerprint) == 0) {
		smbim_ip_configuration_clear(&current);
		return TRUE;
	}
	log_message(daemon->has_ip ?
		"IP configuration changed; replacing netifd dynamic interfaces" :
		"Received initial IP configuration");
	if (!run_helper(&daemon->config, "update", current.json, error)) {
		smbim_ip_configuration_clear(&current);
		return FALSE;
	}
	g_free(daemon->last_fingerprint);
	daemon->last_fingerprint = current.fingerprint;
	current.fingerprint = NULL;
	daemon->has_ip = TRUE;
	smbim_ip_configuration_clear(&current);
	return TRUE;
}

static gboolean refresh_with_recovery(Daemon *daemon, MbimAuthProtocol auth,
				      MbimContextIpType ip_type, char **error)
{
	gboolean missing_required_ip = FALSE;

	if (refresh_ip(daemon, &missing_required_ip, error))
		return TRUE;
	if (!missing_required_ip)
		return FALSE;
	log_message("%s; rebuilding the MBIM data session",
		error && *error ? *error : "required IP address is missing");
	clear_error(error);
	disconnect_verified(daemon);
	if (!ensure_online(daemon, auth, ip_type, error))
		return FALSE;
	return refresh_ip(daemon, NULL, error);
}

static gboolean reconnect_session(Daemon *daemon, MbimAuthProtocol auth,
				  MbimContextIpType ip_type, char **error)
{
	log_message("MBIM data session reconnect requested");
	disconnect_verified(daemon);
	if (!ensure_online(daemon, auth, ip_type, error))
		return FALSE;
	return refresh_with_recovery(daemon, auth, ip_type, error);
}

static gboolean parse_auth(const char *value, MbimAuthProtocol *auth, char **error)
{
	if (!value || !*value || g_ascii_strcasecmp(value, "none") == 0)
		*auth = MBIM_AUTH_PROTOCOL_NONE;
	else if (g_ascii_strcasecmp(value, "pap") == 0)
		*auth = MBIM_AUTH_PROTOCOL_PAP;
	else if (g_ascii_strcasecmp(value, "chap") == 0)
		*auth = MBIM_AUTH_PROTOCOL_CHAP;
	else
		return fail_with(error, "unsupported authentication protocol %s", value);
	return TRUE;
}

static gboolean parse_ip_type(const char *value, MbimContextIpType *ip_type, char **error)
{
	if (!value || !*value || g_ascii_strcasecmp(value, "default") == 0)
		*ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
	else if (g_ascii_strcasecmp(value, "ipv4") == 0)
		*ip_type = MBIM_CONTEXT_IP_TYPE_IPV4;
	else if (g_ascii_strcasecmp(value, "ipv6") == 0)
		*ip_type = MBIM_CONTEXT_IP_TYPE_IPV6;
	else if (g_ascii_strcasecmp(value, "ipv4v6") == 0)
		*ip_type = MBIM_CONTEXT_IP_TYPE_IPV4V6;
	else
		return fail_with(error, "unsupported PDP type %s", value);
	return TRUE;
}

static gboolean valid_interface_name(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;

	if (!value || !*value || strlen(value) > 64)
		return FALSE;
	for (; *cursor; cursor++) {
		if (!(g_ascii_isalnum(*cursor) || *cursor == '_' || *cursor == '-' || *cursor == '.'))
			return FALSE;
	}
	return TRUE;
}

static gboolean valid_pin(const char *value)
{
	const unsigned char *cursor = (const unsigned char *)value;
	gsize length;

	if (!value || !*value)
		return TRUE;
	length = strlen(value);
	if (length < 4 || length > 8)
		return FALSE;
	for (; *cursor; cursor++) {
		if (!g_ascii_isdigit(*cursor))
			return FALSE;
	}
	return TRUE;
}

static gboolean valid_dns_list(const char *value)
{
	char **tokens;
	guint count = 0;
	guint i;
	gboolean valid = TRUE;

	if (!value || !*value)
		return TRUE;
	tokens = g_strsplit_set(value, " \t\r\n", -1);
	for (i = 0; tokens[i]; i++) {
		struct in_addr ipv4;
		struct in6_addr ipv6;

		if (!*tokens[i])
			continue;
		count++;
		if (count > 16 ||
		    (inet_pton(AF_INET, tokens[i], &ipv4) != 1 &&
		     inet_pton(AF_INET6, tokens[i], &ipv6) != 1)) {
			valid = FALSE;
			break;
		}
	}
	g_strfreev(tokens);
	return valid;
}

static gboolean validate_config(const Config *config, char **error)
{
	if (!config->interface || !*config->interface || !config->device || !*config->device ||
	    !config->apn || !*config->apn)
		return fail_with(error, "interface, device, and apn are required");
	if (!g_path_is_absolute(config->device))
		return fail_with(error, "device must be an absolute path");
	if (!valid_interface_name(config->interface))
		return fail_with(error, "interface contains invalid characters or is too long");
	if (!g_path_is_absolute(config->update_helper))
		return fail_with(error, "update helper must be an absolute path");
	if (strlen(config->device) > 4096 || strlen(config->update_helper) > 4096 ||
	    strlen(config->apn) > 255 || strlen(config->username) > 255 ||
	    strlen(config->password) > 255 || strlen(config->custom_dns) > 4096)
		return fail_with(error, "one or more configuration strings are too long");
	if (!valid_pin(config->pin))
		return fail_with(error, "PIN must contain 4 to 8 decimal digits");
	if (!valid_dns_list(config->custom_dns))
		return fail_with(error, "DNS list must contain at most 16 IPv4 or IPv6 addresses");
	if (config->retries < 1 || config->retries > 20)
		return fail_with(error, "retries must be between 1 and 20");
	if (config->poll_interval < 1 || config->poll_interval > 86400)
		return fail_with(error, "poll interval must be between 1 and 86400 seconds");
	if (config->metric < 0 || config->mtu < 0 ||
	    (config->mtu > 0 && (config->mtu < 576 || config->mtu > 9200)))
		return fail_with(error, "metric and MTU values are out of range");
	return TRUE;
}

static void config_clear(Config *config)
{
	volatile unsigned char *cursor;
	gsize length;

	g_free(config->interface);
	g_free(config->device);
	g_free(config->ifname);
	g_free(config->apn);
	length = config->pin ? strlen(config->pin) : 0;
	for (cursor = (volatile unsigned char *)config->pin; length > 0; length--)
		*cursor++ = 0;
	g_free(config->pin);
	g_free(config->auth);
	length = config->username ? strlen(config->username) : 0;
	for (cursor = (volatile unsigned char *)config->username; length > 0; length--)
		*cursor++ = 0;
	g_free(config->username);
	length = config->password ? strlen(config->password) : 0;
	for (cursor = (volatile unsigned char *)config->password; length > 0; length--)
		*cursor++ = 0;
	g_free(config->password);
	g_free(config->ip_type);
	g_free(config->update_helper);
	g_free(config->custom_dns);
	*config = (Config){0};
}

static gboolean parse_options(Config *config, int *argc, char ***argv, char **error)
{
	GError *option_error = NULL;
	GOptionContext *context;
	GOptionEntry entries[] = {
		{ "interface", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->interface, "netifd logical interface", "NAME" },
		{ "device", 0, 0, G_OPTION_ARG_FILENAME, (gpointer)&config->device, "MBIM control device", "PATH" },
		{ "ifname", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->ifname, "WWAN network interface", "NAME" },
		{ "apn", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->apn, "access point name", "APN" },
		{ "pin", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->pin, "SIM PIN", "PIN" },
		{ "auth", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->auth, "authentication protocol", "TYPE" },
		{ "username", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->username, "authentication username", "NAME" },
		{ "password", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->password, "authentication password", "PASSWORD" },
		{ "ip-type", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->ip_type, "PDP type", "TYPE" },
		{ "no-proxy", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &config->proxy, "disable mbim-proxy", NULL },
		{ "retries", 0, 0, G_OPTION_ARG_INT, &config->retries, "attempts per MBIM operation", "COUNT" },
		{ "allow-roaming", 0, 0, G_OPTION_ARG_NONE, &config->allow_roaming, "allow roaming registration", NULL },
		{ "allow-partner", 0, 0, G_OPTION_ARG_NONE, &config->allow_partner, "allow partner registration", NULL },
		{ "poll-interval", 0, 0, G_OPTION_ARG_INT, &config->poll_interval, "IP sanity poll interval in seconds", "SECONDS" },
		{ "update-helper", 0, 0, G_OPTION_ARG_FILENAME, (gpointer)&config->update_helper, "netifd update helper", "PATH" },
		{ "no-default-route", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &config->default_route, "disable default route", NULL },
		{ "metric", 0, 0, G_OPTION_ARG_INT, &config->metric, "route metric", "VALUE" },
		{ "no-peer-dns", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &config->peer_dns, "ignore modem DNS", NULL },
		{ "dns", 0, 0, G_OPTION_ARG_STRING, (gpointer)&config->custom_dns, "space separated custom DNS servers", "LIST" },
		{ "no-delegate", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &config->delegate, "disable IPv6 delegation", NULL },
		{ "no-source-filter", 0, G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &config->source_filter, "disable IPv6 source filtering", NULL },
		{ "mtu", 0, 0, G_OPTION_ARG_INT, &config->mtu, "override MTU", "VALUE" },
		{ NULL }
	};

	config->proxy = TRUE;
	config->default_route = TRUE;
	config->peer_dns = TRUE;
	config->delegate = TRUE;
	config->source_filter = TRUE;
	config->retries = 5;
	config->poll_interval = 30;
	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, entries, NULL);
	if (!g_option_context_parse(context, argc, argv, &option_error)) {
		fail_with(error, "%s", option_error->message);
		g_clear_error(&option_error);
		g_option_context_free(context);
		return FALSE;
	}
	g_option_context_free(context);
	if (*argc != 1)
		return fail_with(error, "unexpected positional arguments");
	if (!config->auth)
		config->auth = g_strdup("none");
	if (!config->ip_type)
		config->ip_type = g_strdup("ipv4v6");
	if (!config->update_helper)
		config->update_helper = g_strdup("/usr/libexec/smbim-netifd");
	if (!config->custom_dns)
		config->custom_dns = g_strdup("");
	if (!config->pin)
		config->pin = g_strdup(g_getenv("SMBIM_PIN"));
	if (!config->username)
		config->username = g_strdup(g_getenv("SMBIM_USERNAME"));
	if (!config->password)
		config->password = g_strdup(g_getenv("SMBIM_PASSWORD"));
	if (!config->pin)
		config->pin = g_strdup("");
	if (!config->username)
		config->username = g_strdup("");
	if (!config->password)
		config->password = g_strdup("");
	g_unsetenv("SMBIM_PIN");
	g_unsetenv("SMBIM_USERNAME");
	g_unsetenv("SMBIM_PASSWORD");
	return TRUE;
}

static int daemon_run(Daemon *daemon, MbimAuthProtocol auth, MbimContextIpType ip_type)
{
	char *error = NULL;
	gint64 next_poll;
	int exit_code = 1;

	if (!wait_for_device(&daemon->config, &error) || !open_with_retry(daemon, &error))
		goto out;
	log_message("Opened %s through libmbim (proxy=%s)", daemon->config.device,
		daemon->config.proxy ? "true" : "false");
	if (!smbim_subscribe(daemon->client, &error)) {
		char *wrapped = g_strdup_printf("subscribe to MBIM indications: %s",
			error ? error : "unknown error");
		clear_error(&error);
		error = wrapped;
		goto out;
	}
	log_message("Subscribed to register, packet service, connect, and IP configuration indications");
	if (!prepare_sim(daemon, &error) || !ensure_online(daemon, auth, ip_type, &error) ||
	    !refresh_with_recovery(daemon, auth, ip_type, &error))
		goto out;
	next_poll = g_get_monotonic_time() + (gint64)daemon->config.poll_interval * G_USEC_PER_SEC;
	while (!stop_requested) {
		char *detail = NULL;
		int event = smbim_poll_event(daemon->client, 1000, &detail);
		if (detail && *detail)
			log_message("%s", detail);
		switch (event) {
		case SMBIM_EVENT_IP_CONFIGURATION:
			if (!refresh_with_recovery(daemon, auth, ip_type, &error)) {
				log_message("IP configuration refresh failed: %s", error);
				clear_error(&error);
			}
			break;
		case SMBIM_EVENT_CONNECT:
		case SMBIM_EVENT_PACKET_SERVICE:
		case SMBIM_EVENT_REGISTER_STATE:
			if (!ensure_online(daemon, auth, ip_type, &error)) {
				g_free(detail);
				goto out;
			}
			if (!refresh_with_recovery(daemon, auth, ip_type, &error)) {
				log_message("IP configuration refresh after state change failed: %s", error);
				clear_error(&error);
			}
			break;
		case SMBIM_EVENT_DEVICE_ERROR:
			error = g_strdup_printf("MBIM device error: %s", detail ? detail : "unknown error");
			g_free(detail);
			goto out;
		case SMBIM_EVENT_DEVICE_REMOVED:
			error = g_strdup("MBIM device was removed");
			g_free(detail);
			goto out;
		default:
			break;
		}
		g_free(detail);
		if (reconnect_requested) {
			reconnect_requested = 0;
			if (!reconnect_session(daemon, auth, ip_type, &error))
				goto out;
		}
		if (g_get_monotonic_time() >= next_poll) {
			if (!refresh_with_recovery(daemon, auth, ip_type, &error)) {
				log_message("Periodic IP configuration query failed: %s", error);
				clear_error(&error);
			}
			next_poll = g_get_monotonic_time() +
				(gint64)daemon->config.poll_interval * G_USEC_PER_SEC;
		}
	}
	log_message("Shutdown requested");
	exit_code = 0;

out:
	if (exit_code != 0 && error)
		log_message("Fatal error: %s", error);
	clear_error(&error);
	if (daemon->client) {
		if (stop_requested)
			disconnect_verified(daemon);
		if (!run_helper(&daemon->config, "down", "", &error)) {
			log_message("netifd cleanup failed: %s", error);
			clear_error(&error);
		}
		smbim_client_close(daemon->client);
		daemon->client = NULL;
	}
	return exit_code;
}

int main(int argc, char **argv)
{
	Daemon daemon = {0};
	MbimAuthProtocol auth = MBIM_AUTH_PROTOCOL_NONE;
	MbimContextIpType ip_type = MBIM_CONTEXT_IP_TYPE_DEFAULT;
	char *error = NULL;
	struct sigaction action = {0};
	struct sigaction reconnect_action = {0};
	int exit_code;

	action.sa_handler = handle_shutdown_signal;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGTERM, &action, NULL) != 0 || sigaction(SIGINT, &action, NULL) != 0) {
		log_message("Fatal error: cannot install signal handler: %s", g_strerror(errno));
		return 1;
	}
	reconnect_action.sa_handler = handle_reconnect_signal;
	sigemptyset(&reconnect_action.sa_mask);
	if (sigaction(SIGHUP, &reconnect_action, NULL) != 0) {
		log_message("Fatal error: cannot install reconnect signal handler: %s", g_strerror(errno));
		return 1;
	}
	if (!parse_options(&daemon.config, &argc, &argv, &error) ||
	    !validate_config(&daemon.config, &error) ||
	    !parse_auth(daemon.config.auth, &auth, &error) ||
	    !parse_ip_type(daemon.config.ip_type, &ip_type, &error)) {
		log_message("Configuration error: %s", error ? error : "invalid configuration");
		clear_error(&error);
		config_clear(&daemon.config);
		return 2;
	}
	exit_code = daemon_run(&daemon, auth, ip_type);
	g_free(daemon.last_fingerprint);
	config_clear(&daemon.config);
	return exit_code;
}
