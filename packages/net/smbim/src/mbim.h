#ifndef SMBIM_MBIM_H
#define SMBIM_MBIM_H

typedef struct SmbimClient SmbimClient;

typedef struct {
	char *json;
	char *fingerprint;
	int has_addresses;
	int has_ipv4;
	int has_ipv6;
} SmbimIpConfiguration;

enum {
	SMBIM_EVENT_NONE = 0,
	SMBIM_EVENT_IP_CONFIGURATION = 1,
	SMBIM_EVENT_CONNECT = 2,
	SMBIM_EVENT_PACKET_SERVICE = 3,
	SMBIM_EVENT_REGISTER_STATE = 4,
	SMBIM_EVENT_DEVICE_ERROR = 5,
	SMBIM_EVENT_DEVICE_REMOVED = 6,
};

SmbimClient *smbim_client_open(const char *device, int use_proxy, unsigned int retries, char **error);
void smbim_client_close(SmbimClient *client);
int smbim_subscribe(SmbimClient *client, char **error);
int smbim_pin_status(SmbimClient *client, int *pin_type, int *pin_state, char **error);
int smbim_unlock_pin_once(SmbimClient *client, const char *pin, unsigned int attempt, char **error);
int smbim_subscriber_status(SmbimClient *client, int *ready_state, char **error);
int smbim_register_status(SmbimClient *client, int *register_state, char **error);
int smbim_packet_status(SmbimClient *client, int *packet_state, char **error);
int smbim_attach_once(SmbimClient *client, unsigned int attempt, char **error);
int smbim_connect_status(SmbimClient *client, int *activation_state, int *ip_type, char **error);
int smbim_connect_once(SmbimClient *client, const char *apn, const char *username,
		       const char *password, int auth, int ip_type, unsigned int attempt, char **error);
int smbim_disconnect_once(SmbimClient *client, unsigned int attempt, char **error);
int smbim_query_ip_configuration(SmbimClient *client, SmbimIpConfiguration *config, char **error);
void smbim_ip_configuration_clear(SmbimIpConfiguration *config);
int smbim_poll_event(SmbimClient *client, unsigned int timeout_ms, char **detail);
void smbim_string_free(char *value);

#endif
