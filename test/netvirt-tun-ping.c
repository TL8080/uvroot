/*
 * End-to-end check for the virtual /dev/net/tun: create a tun inside the
 * container, configure it, send an ICMP echo request through the
 * WireGuard bridge and wait for the reply.
 *
 * Usage: netvirt-tun-ping <own-address> <peer-address> [device]
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef TUNSETIFF
#define TUNSETIFF 0x400454ca
#endif
#define IFF_TUN 0x0001
#define IFF_NO_PI 0x1000

static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i + 1 < length; i += 2)
	sum += ((uint32_t) data[i] << 8) | data[i + 1];
    if (i < length)
	sum += (uint32_t) data[i] << 8;
    while (sum >> 16)
	sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t) ~sum;
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t) (value >> 8);
    p[1] = (uint8_t) value;
}

static int configure(int control, const char *device, const char *address,
		     const char *netmask)
{
    struct ifreq request;
    struct sockaddr_in *value;

    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, IFNAMSIZ, "%s", device);

    value = (struct sockaddr_in *) &request.ifr_addr;
    value->sin_family = AF_INET;
    inet_pton(AF_INET, address, &value->sin_addr);
    if (ioctl(control, SIOCSIFADDR, &request) < 0) {
	perror("SIOCSIFADDR");
	return -1;
    }

    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, IFNAMSIZ, "%s", device);
    value = (struct sockaddr_in *) &request.ifr_netmask;
    value->sin_family = AF_INET;
    inet_pton(AF_INET, netmask, &value->sin_addr);
    if (ioctl(control, SIOCSIFNETMASK, &request) < 0) {
	perror("SIOCSIFNETMASK");
	return -1;
    }

    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, IFNAMSIZ, "%s", device);
    if (ioctl(control, SIOCGIFFLAGS, &request) < 0) {
	perror("SIOCGIFFLAGS");
	return -1;
    }
    request.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(control, SIOCSIFFLAGS, &request) < 0) {
	perror("SIOCSIFFLAGS");
	return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *device = (argc > 3) ? argv[3] : "vtun";
    const char *own = (argc > 1) ? argv[1] : "10.9.0.2";
    const char *peer = (argc > 2) ? argv[2] : "10.9.0.1";
    uint8_t packet[84];
    uint8_t reply[2048];
    struct ifreq request;
    struct pollfd descriptor;
    int tun;
    int control;
    int length;

    tun = open("/dev/net/tun", O_RDWR);
    if (tun < 0) {
	perror("open(/dev/net/tun)");
	return 1;
    }
    memset(&request, 0, sizeof(request));
    snprintf(request.ifr_name, IFNAMSIZ, "%s", device);
    request.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (ioctl(tun, TUNSETIFF, &request) < 0) {
	perror("ioctl(TUNSETIFF)");
	return 1;
    }
    printf("tun=%s\n", request.ifr_name);
    fflush(stdout);

    control = socket(AF_INET, SOCK_DGRAM, 0);
    if (control < 0 || configure(control, device, own, "255.255.255.0") < 0)
	return 1;

    /* IPv4 + ICMP echo request.  */
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x45;
    put_be16(packet + 2, sizeof(packet));
    packet[8] = 64;
    packet[9] = 1;		/* ICMP */
    inet_pton(AF_INET, own, packet + 12);
    inet_pton(AF_INET, peer, packet + 16);
    put_be16(packet + 10, checksum(packet, 20));

    packet[20] = 8;		/* echo request */
    packet[24] = 0x12;
    packet[25] = 0x34;
    packet[26] = 0x00;
    packet[27] = 0x01;
    memset(packet + 28, 0x41, sizeof(packet) - 28);
    put_be16(packet + 22, checksum(packet + 20, sizeof(packet) - 20));

    if (write(tun, packet, sizeof(packet)) != (ssize_t) sizeof(packet)) {
	perror("write(tun)");
	return 1;
    }

    descriptor.fd = tun;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, 5000) <= 0) {
	fprintf(stderr, "no reply through the tunnel\n");
	return 1;
    }
    length = (int) read(tun, reply, sizeof(reply));
    if (length < 28) {
	fprintf(stderr, "short reply (%d bytes)\n", length);
	return 1;
    }
    if (reply[9] != 1 || reply[20] != 0) {
	fprintf(stderr, "not an ICMP echo reply (proto %u type %u)\n",
		reply[9], reply[20]);
	return 1;
    }
    if (reply[24] != 0x12 || reply[25] != 0x34) {
	fprintf(stderr, "wrong ICMP identifier\n");
	return 1;
    }

    printf("TUN-PING OK (%d bytes)\n", length);
    return 0;
}
