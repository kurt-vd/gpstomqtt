/*
 * Copyright 2018 Kurt Van Dijck <dev.kurt@vandijck-laurijssen.be>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <getopt.h>
#include <syslog.h>
#include <mosquitto.h>
#include <sys/signalfd.h>
#include <sys/uio.h>

#define NAME "nmea-snr"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define LOG_EXIT	0x4000000

/* safeguard our LOG_EXIT extension */
#if (LOG_EXIT & (LOG_FACMASK | LOG_PRIMASK))
#error LOG_EXIT conflict
#endif

static int logtostderr = -1;

static char *nowstr(void)
{
	struct timespec tv;
	static char timbuf[64];

	clock_gettime(CLOCK_REALTIME, &tv);
	strftime(timbuf, sizeof(timbuf), "%b %d %H:%M:%S", localtime(&tv.tv_sec));
	sprintf(timbuf+strlen(timbuf), ".%03u ", (int)(tv.tv_nsec/1000000));
	return timbuf;
}

void mylog(int loglevel, const char *fmt, ...)
{
	va_list va;
	char *msg = NULL;

	if (logtostderr < 0)
		logtostderr = abs(isatty(STDERR_FILENO));

	if (logtostderr) {
		char *timbuf = nowstr();

		va_start(va, fmt);
		vasprintf(&msg, fmt, va);
		va_end(va);

		struct iovec vec[] = {
			{ .iov_base = timbuf, .iov_len = strlen(timbuf), },
			{ .iov_base = NAME, .iov_len = strlen(NAME), },
			{ .iov_base = ": ", .iov_len = 2, },
			{ .iov_base = msg, .iov_len = strlen(msg), },
			{ .iov_base = "\n", .iov_len = 1, },
		};
		writev(STDERR_FILENO, vec, sizeof(vec)/sizeof(vec[0]));
		free(msg);
	} else {
		va_start(va, fmt);
		vsyslog(loglevel & LOG_PRIMASK, fmt, va);
		va_end(va);
	}
	if (loglevel & LOG_EXIT)
		exit(1);
}

#define ESTR(num)	strerror(num)

/* program options */
static const char help_msg[] =
	NAME ": show NMEA snr's from MQTT\n"
	"usage:	" NAME " [OPTIONS ...]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -h, --host=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -p, --prefix=PREFIX	Prefix MQTT topics, including final slash, default to 'gps/'\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "host", required_argument, NULL, 'h', },
	{ "prefix", required_argument, NULL, 'p', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?h:p:";

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = -1;

static const char *topicprefix = "gps/";
static int topicprefixlen = 4;

/* state */
static struct mosquitto *mosq;

/* GSV: keep track of satellites */
/* range of sat. ids:
 * 1..32: GPS
 * 33..54: SBAS
 * 64..88/96: GLONASS
 * 193..195: QZSS
 * 201..235: Beidou
 * 301..336: Galileo
 */
#define NSATS	512
struct sat {
	int snr;
	int8_t recvd; /* recvd from NMEA */
	char talker[3];
};
static struct sat sats[NSATS];
static int maxsat;
static int changes;

static void print_snr(void)
{
	int j, n;
	struct sat *sat;

	if (!changes)
		return;

	printf("%s", nowstr());
	for (j = n = 0, sat = sats; j <= maxsat; ++j, ++sat) {
		if (!sat->recvd)
			continue;
		++n;
		printf("%s%s%u:%u", n ? "\t" : "", sat->talker, j, sat->snr);
	}
	changes = 0;
	if (!n)
		printf("no satellites");
	printf("\n");
	fflush(stdout);
}

/* MQTT API */
static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	char *str;
	int ret;

	str = strrchr(msg->topic, '/');
	if (!str)
		return;
	++str;
	if (!strcmp("alive", str)) {
		ret = strtoul(msg->payload ?: "", NULL, 0);
		mylog(LOG_WARNING, "gps %s", ret ? "alive" : "dead");

	} else if (!msg->retained && !strcmp("satview", str)) {
		alarm(0);
		print_snr();

	} else if (!strcmp("snr", str)) {
		/* parse SNR */
		char *talker = strtok(msg->topic+topicprefixlen, "/");
		strtok(NULL, "/"); /* 'sat' */
		int prn = strtoul(strtok(NULL, "/") ?: "0", NULL, 0);
		int snr = strtoul((char *)msg->payload ?: "-1", NULL, 0);
		int recvd = snr >= 0;

		changes += (snr != sats[prn].snr) || (recvd != sats[prn].recvd);
		strncpy(sats[prn].talker, talker, 3);
		sats[prn].snr = snr;
		sats[prn].recvd = recvd;
		if (prn > maxsat)
			maxsat = prn;
	}
}

static void sigalrm(int sig)
{
	mylog(LOG_NOTICE, "not data, do you need to send '%scfg/msgs' '+gsv'", topicprefix);
}

int main(int argc, char *argv[])
{
	int opt, ret;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);

	/* argument parsing */
	while ((opt = getopt_long(argc, argv, optstring, long_opts, NULL)) >= 0)
	switch (opt) {
	case 'V':
		fprintf(stderr, "%s %s\nCompiled on %s %s\n",
				NAME, VERSION, __DATE__, __TIME__);
		exit(0);
	case 'v':
		switch (logmask) {
		case LOG_UPTO(LOG_NOTICE):
			logmask = LOG_UPTO(LOG_INFO);
			break;
		case LOG_UPTO(LOG_INFO):
			logmask = LOG_UPTO(LOG_DEBUG);
			break;
		}
		break;
	case 'h':
		mqtt_host = optarg;
		str = strrchr(optarg, ':');
		if (str > mqtt_host && *(str-1) != ']') {
			/* TCP port provided */
			*str = 0;
			mqtt_port = strtoul(str+1, NULL, 10);
		}
		break;
	case 'p':
		topicprefix = optarg;
		topicprefixlen = strlen(topicprefix);
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	setlogmask(logmask);

	if (mqtt_qos < 0)
		mqtt_qos = !strcmp(mqtt_host ?: "", "localhost") ? 0 : 1;
	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, NULL);
	if (!mosq)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_new failed: %s", ESTR(errno));

	char *willtopic;
	asprintf(&willtopic, "%salive", topicprefix);
	ret = mosquitto_will_set(mosq, willtopic, 7, "crashed", mqtt_qos, 1);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_will_set: %s", mosquitto_strerror(ret));
	free(willtopic);

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	asprintf(&str, "%s+/sat/+/snr", topicprefix);
	ret = mosquitto_subscribe(mosq, NULL, str, mqtt_qos);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_subscribe %s: %s", str, mosquitto_strerror(ret));
	mylog(LOG_INFO, "subscribed to %s", str);
	free(str);
	asprintf(&str, "%s+/satview", topicprefix);
	ret = mosquitto_subscribe(mosq, NULL, str, mqtt_qos);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_subscribe %s: %s", str, mosquitto_strerror(ret));
	mylog(LOG_INFO, "subscribed to %s", str);
	free(str);
	asprintf(&str, "%salive", topicprefix);
	ret = mosquitto_subscribe(mosq, NULL, str, mqtt_qos);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_subscribe %s: %s", str, mosquitto_strerror(ret));
	mylog(LOG_INFO, "subscribed to %s", str);
	free(str);

	/* schedule no data alarm */
	signal(SIGALRM, sigalrm);
	alarm(5);

	ret = mosquitto_loop_forever(mosq, 1000, 1);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_loop: %s", mosquitto_strerror(ret));
	return 0;
}
