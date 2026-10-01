/*
 * Probe used by test-netvirt.sh.  It must be run through
 * "uvroot -r / --net ..." and checks that the container sees exactly
 * the two mapped devices (plus the loopback) through every interface
 * the libc offers: getifaddrs(3) (rtnetlink), the legacy SIOCGIF*
 * ioctls, and a raw NETLINK_ROUTE dump.
 *
 * It prints "NETVIRT-PROBE OK" and returns 0 when everything matches.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static int failures = 0;

static void check(int condition, const char *message)
{
    if (!condition) {
	fprintf(stderr, "FAIL: %s\n", message);
	failures++;
    }
}

static int count_links(void)
{
    int fd;
    struct sockaddr_nl local;
    struct {
	struct nlmsghdr header;
	struct ifinfomsg info;
    } request;
    int links = 0;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
	return -1;

    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0) {
	close(fd);
	return -1;
    }

    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = sizeof(request);
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.info.ifi_family = AF_UNSPEC;

    if (send(fd, &request, sizeof(request), 0) < 0) {
	close(fd);
	return -1;
    }

    for (;;) {
	unsigned char buffer[8192];
	ssize_t length = recv(fd, buffer, sizeof(buffer), 0);
	struct nlmsghdr *header;
	int remaining;
	int done = 0;

	if (length < 0) {
	    perror("recv");
	    close(fd);
	    return -1;
	}

	header = (struct nlmsghdr *) buffer;
	remaining = (int) length;
	while (NLMSG_OK(header, remaining)) {
	    if (header->nlmsg_type == NLMSG_DONE) {
		done = 1;
		break;
	    }
	    if (header->nlmsg_type == RTM_NEWLINK)
		links++;
	    header = NLMSG_NEXT(header, remaining);
	}
	if (done)
	    break;
    }

    close(fd);
    return links;
}

int main(void)
{
    struct ifaddrs *list = NULL;
    struct ifaddrs *entry;
    int seen_veth0 = 0;
    int seen_vtun = 0;
    int seen_loopback = 0;
    int veth0_address = 0;
    int vtun_address = 0;
    int links;

    /* getifaddrs(3): glibc resolves it over rtnetlink.  */
    check(getifaddrs(&list) == 0, "getifaddrs() failed");
    for (entry = list; entry != NULL; entry = entry->ifa_next) {
	if (strcmp(entry->ifa_name, "veth0") == 0)
	    seen_veth0 = 1;
	else if (strcmp(entry->ifa_name, "vtun") == 0)
	    seen_vtun = 1;
	else if (strcmp(entry->ifa_name, "lo") == 0)
	    seen_loopback = 1;
	else {
	    fprintf(stderr, "FAIL: unexpected interface %s\n",
		    entry->ifa_name);
	    failures++;
	}

	if (entry->ifa_addr != NULL
	    && entry->ifa_addr->sa_family == AF_INET) {
	    char text[INET_ADDRSTRLEN];
	    struct sockaddr_in *address =
		(struct sockaddr_in *) entry->ifa_addr;

	    inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
	    if (strcmp(entry->ifa_name, "veth0") == 0
		&& strcmp(text, "10.177.0.2") == 0)
		veth0_address = 1;
	    if (strcmp(entry->ifa_name, "vtun") == 0
		&& strcmp(text, "100.64.0.2") == 0)
		vtun_address = 1;
	}
    }
    if (list != NULL)
	freeifaddrs(list);

    check(seen_veth0, "getifaddrs() does not report veth0");
    check(seen_vtun, "getifaddrs() does not report vtun");
    check(seen_loopback, "getifaddrs() does not report lo");
    check(veth0_address, "veth0 has no 10.177.0.2 address");
    check(vtun_address, "vtun has no 100.64.0.2 address");

    /* Legacy ioctls.  */
    {
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifconf configuration;
	char buffer[4096];
	int count;

	check(fd >= 0, "socket(AF_INET) failed");
	if (fd >= 0) {
	    configuration.ifc_len = sizeof(buffer);
	    configuration.ifc_buf = buffer;
	    check(ioctl(fd, SIOCGIFCONF, &configuration) == 0,
		  "SIOCGIFCONF failed");
	    count = configuration.ifc_len / sizeof(struct ifreq);
	    check(count == 3, "SIOCGIFCONF does not report 3 interfaces");

	    {
		struct ifreq request;

		memset(&request, 0, sizeof(request));
		snprintf(request.ifr_name, IFNAMSIZ, "veth0");
		check(ioctl(fd, SIOCGIFINDEX, &request) == 0
		      && request.ifr_ifindex == 2,
		      "SIOCGIFINDEX(veth0) is not 2");
		check(ioctl(fd, SIOCGIFMTU, &request) == 0
		      && request.ifr_mtu == 1500,
		      "SIOCGIFMTU(veth0) is not 1500");
	    }
	    close(fd);
	}
    }

    check(if_nametoindex("veth0") == 2, "if_nametoindex(veth0) is not 2");
    check(if_nametoindex("vtun") == 3, "if_nametoindex(vtun) is not 3");

    /* Raw rtnetlink dump.  */
    links = count_links();
    check(links == 3, "RTM_GETLINK did not report 3 links");

    if (failures != 0) {
	fprintf(stderr, "NETVIRT-PROBE FAILED (%d)\n", failures);
	return 1;
    }
    printf("NETVIRT-PROBE OK\n");
    return 0;
}
