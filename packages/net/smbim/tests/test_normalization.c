#include <arpa/inet.h>
#include <assert.h>
#include <string.h>

#include "../src/mbim.c"

static void test_ipv6_prefix_normalization(void)
{
	struct in6_addr first;
	struct in6_addr second;
	struct in6_addr different;
	char *first_prefix;
	char *second_prefix;
	char *different_prefix;

	assert(inet_pton(AF_INET6, "2001:b400:e24e:ecf6:1::1", &first) == 1);
	assert(inet_pton(AF_INET6, "2001:b400:e24e:ecf6:ffff::2", &second) == 1);
	assert(inet_pton(AF_INET6, "2001:b400:e24e:ed00::2", &different) == 1);
	first_prefix = format_address(first.s6_addr, AF_INET6, 64, TRUE, TRUE);
	second_prefix = format_address(second.s6_addr, AF_INET6, 64, TRUE, TRUE);
	different_prefix = format_address(different.s6_addr, AF_INET6, 64, TRUE, TRUE);
	assert(first_prefix && second_prefix && different_prefix);
	assert(strcmp(first_prefix, second_prefix) == 0);
	assert(strcmp(first_prefix, different_prefix) != 0);
	g_free(first_prefix);
	g_free(second_prefix);
	g_free(different_prefix);
}

static void test_invalid_prefix_is_rejected(void)
{
	struct in6_addr address;

	assert(inet_pton(AF_INET6, "2001:db8::1", &address) == 1);
	assert(format_address(address.s6_addr, AF_INET6, 129, TRUE, TRUE) == NULL);
	assert(format_address(address.s6_addr, AF_UNSPEC, 0, TRUE, TRUE) == NULL);
}

static void test_event_queue_is_coalesced_and_bounded(void)
{
	SmbimClient client = {0};
	guint i;

	client.events = g_queue_new();
	for (i = 0; i < 1000; i++)
		queue_event(&client, SMBIM_EVENT_IP_CONFIGURATION, "changed");
	assert(g_queue_get_length(client.events) == 1);
	queue_event(&client, SMBIM_EVENT_CONNECT, "connected");
	assert(g_queue_get_length(client.events) == 2);
	while (!g_queue_is_empty(client.events)) {
		QueuedEvent *event = g_queue_pop_head(client.events);
		g_free(event->detail);
		g_free(event);
	}
	g_queue_free(client.events);
}

int main(void)
{
	test_ipv6_prefix_normalization();
	test_invalid_prefix_is_rejected();
	test_event_queue_is_coalesced_and_bounded();
	return 0;
}
