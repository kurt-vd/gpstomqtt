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
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <syslog.h>
#include <mosquitto.h>
#include <sys/signalfd.h>

#define NAME "nmea0183tomqtt"
#ifndef VERSION
#define VERSION "<undefined version>"
#endif

/* generic error logging */
#define mylog(loglevel, fmt, ...) \
	({\
		syslog(loglevel, fmt, ##__VA_ARGS__); \
		if (loglevel <= LOG_ERR)\
			exit(1);\
	})
#define ESTR(num)	strerror(num)

/* program options */
static const char help_msg[] =
	NAME ": Propagate nmea0183 input to MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [FILE|DEVICE]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -h, --host=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -n, --nmea=GGA[,ZDA...]	Specify what message to forward\n"
	"		Possible messages are:\n"
	"		*GGA	lon, lat, alt, hdop, quality\n"
	"		 GSA	DOP & active satellites\n"
	"		 GSV	Satellites in view info\n"
	"		*VTG	Speed & heading\n"
	"		*ZDA	GPS time\n"
	"		Default: GGA,ZDA,VTG\n"
	" -a, --always		Publish everything on reception, always\n"
	"			By default, publish topics only when something changed in the same block\n"
	"			This holds the program from publishing until something really changed\n"
	"			Upon change, all topics in that NMEA0183 sentence are published\n"
	"			to yield a coherent dataset\n"
	"			Adding -a may produce a lot of equal noise\n"
	" -p, --prefix=PREFIX	Prefix MQTT topics, including final slash, default to 'gps/'\n"
	" -D, --deadtime=DELAY	Consider port dead after DELAY seconds of silence (default 10)\n"
	"\n"
	"Arguments\n"
	" FILE|DEVICE	Read input from FILE or DEVICE\n"
	;

#ifdef _GNU_SOURCE
static struct option long_opts[] = {
	{ "help", no_argument, NULL, '?', },
	{ "version", no_argument, NULL, 'V', },
	{ "verbose", no_argument, NULL, 'v', },

	{ "host", required_argument, NULL, 'h', },
	{ "nmea", required_argument, NULL, 'n', },
	{ "prefix", required_argument, NULL, 'p', },
	{ "always", no_argument, NULL, 'a', },
	{ "deadtime", required_argument, NULL, 'd', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?h:n:p:ad:";

/* signal handler */
static volatile int sigterm;
static volatile int sigalrm;
static volatile int ready;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = -1;

/* state */
static struct mosquitto *mosq;

static const char *nmea_use = "gga,zda,vtg";
static const char *topicprefix = "gps/";
static int always;
static int deaddelay = 10;
static int portalive = -1;

static char talker[3] = {};

/* nmea tables */
static const char *const strquality[] = {
	[0] = "none",
	[1] = "gps",
	[2] = "dgps",
	[3] = "pps",
	[4] = "rtk",
	[5] = "float-rtk",
	[6] = "estimated",
	[7] = "manual input",
	[8] = "simulation",
};
static const char *const strmode[] = {
	[1] = "no fix",
	[2] = "2D",
	[3] = "3D",
};

#define fromtable(table, idx)	(((idx) >= sizeof(table)/sizeof((table)[0])) ? NULL : (table)[idx])

static void my_exit(void)
{
	if (mosq)
		mosquitto_disconnect(mosq);
}

/* MQTT API */
static char *myuuid;
static const char selfsynctopic[] = "tmp/selfsync";
static void send_self_sync(struct mosquitto *mosq)
{
	int ret;

	asprintf(&myuuid, "%i-%li-%i", getpid(), time(NULL), rand());

	ret = mosquitto_subscribe(mosq, NULL, selfsynctopic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR, "mosquitto_subscribe %s: %s", selfsynctopic, mosquitto_strerror(ret));
	ret = mosquitto_publish(mosq, NULL, selfsynctopic, strlen(myuuid), myuuid, mqtt_qos, 0);
	if (ret < 0)
		mylog(LOG_ERR, "mosquitto_publish %s: %s", selfsynctopic, mosquitto_strerror(ret));
}
static int is_self_sync(const struct mosquitto_message *msg)
{
	return !strcmp(msg->topic, selfsynctopic) &&
		!strncmp(myuuid ?: "", msg->payload ?: "", msg->payloadlen);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
	if (is_self_sync(msg))
		ready = 1;
}

/* cache per NMEA message */
struct topic {
	struct topic *next;
	int written;
	int retain;
	int ctrltopic;
	char *topic;
	char *payload;
};

static struct topic *topics, *lasttopic;
static int ndirty;
static int in_data_sentence;

#define publish_topic(topic, vfmt, ...) publish_topicr((topic), 1, (vfmt), ##__VA_ARGS__)
__attribute__((format(printf,3,4)))
static void publish_topicr(const char *topic, int retain, const char *vfmt, ...)
{
	va_list va;
	int ret;
	static char value[1024];
	static char realtopic[1024];
	struct topic *it;

	va_start(va, vfmt);
	vsprintf(value, vfmt, va);
	va_end(va);

	if (!strcmp(value, "nan"))
		strcpy(value, "");

	sprintf(realtopic, "%s%s", topicprefix, topic);

	if (!retain) {
		ret = mosquitto_publish(mosq, NULL, realtopic, strlen(value), value, mqtt_qos, retain);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_publish %s: %s", realtopic, mosquitto_strerror(ret));
		return;
	}

	for (it = topics; it; it = it->next) {
		if (!strcmp(it->topic, realtopic))
			break;
	}
	if (!it) {
		it = malloc(sizeof(*it));
		if (!it)
			mylog(LOG_ERR, "malloc failed: %s", ESTR(errno));
		memset(it, 0, sizeof(*it));
		/* append to linked list */
		if (!topics) {
			topics = lasttopic = it;
		} else {
			lasttopic->next = it;
			lasttopic = it;
		}
		it->topic = strdup(realtopic);
		/* save 'retain' only once */
		it->retain = retain;
		it->ctrltopic = !in_data_sentence;
	}
	it->written = 1;
	if (strcmp(it->payload ?: "", value)) {
		if (it->payload)
			free(it->payload);
		it->payload = strdup(value);
		++ndirty;
	}
}

static void flush_pending_topics(void)
{
	struct topic *it;
	int ret;

	for (it = topics; it; it = it->next) {
		/* publish cache */
		if (it->written && (ndirty || always)) {
			ret = mosquitto_publish(mosq, NULL, it->topic, strlen(it->payload ?: ""), it->payload, mqtt_qos, it->retain);
			if (ret < 0)
				mylog(LOG_ERR, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
		}
		it->written = 0;
	}
	ndirty = 0;
}

static void erase_topics(int clrctrl)
{
	struct topic *it;

	for (it = topics; it; it = it->next) {
		if (it->ctrltopic && !clrctrl)
			continue;
		if (!it->payload)
			/* nothting to erase */
			continue;
		/* clear cached value, and mark as dirty */
		free(it->payload);
		it->payload = NULL;
		it->written = 1;
		++ndirty;
	}
	flush_pending_topics();
}

/* nmea parser */
static char *nmea_tok(char *line)
{
	static char *saved_line;
	char *str;

	if (!line)
		line = saved_line;
	else if (*line == '$')
		/* omit leading $ */
		++line;

	for (str = line; *str; ++str) {
		if (strchr(",*", *str)) {
			*str++ = 0;
			saved_line = str;
			return line;
		}
	}
	return *line ? line : NULL;
}
static inline char *nmea_safe_tok(char *line)
{
	return nmea_tok(line) ?: "";
}

/* parse DDDMM.MMMMM to double */
static double nmea_deg_to_double(const char *str)
{
	long lval;
	char *endp;

	if (!*str)
		return NAN;
	lval = strtol(str, &endp, 10);
	return ((lval %100)+ strtod(endp, 0))/60.0 + (lval /100);
}
static inline double nmea_strtod(const char *str)
{
	return *str ? strtod(str, NULL) : NAN;
}

static int nmea_is_valid_sentence(char *line)
{
	char *str;
	uint8_t nmea_sum, my_sum;

	if ('$' != *line) {
		mylog(LOG_WARNING, "bad nmea message '%.10s'", line);
		return -1;
	}

	/* make my sum, start after initial $ */
	for (str = line+1, my_sum = 0; *str; ++str) {
		if (*str == '*') {
			/* end of sentence */
			nmea_sum = strtoul(str+1, NULL, 16);
			if (my_sum != nmea_sum) {
				mylog(LOG_WARNING, "bad sum on nmea msg '%.10s'", line);
				return -1;
			}
			return 0;
		}
		my_sum ^= *str;
	}
	/* no checksum found, that can't be good */
	mylog(LOG_WARNING, "incomplete nmea msg '%.10s'", line);
	return -1;
}

static void recvd_gga(void)
{
	double dval;
	int ival;

	/* omit UTC within day */
	nmea_tok(NULL);
	/* latt */
	dval = nmea_deg_to_double(nmea_safe_tok(NULL));
	/* lat sign */
	if (*nmea_safe_tok(NULL) == 'S')
		dval *= -1;
	publish_topic("lat", "%.7lf", dval);
	/* lon */
	dval = nmea_deg_to_double(nmea_safe_tok(NULL));
	/* lon sign */
	if (*nmea_safe_tok(NULL) == 'W')
		dval *= -1;
	publish_topic("lon", "%.7lf", dval);
	/* fix */
	ival = strtoul(nmea_safe_tok(NULL), NULL, 10);
	publish_topic("quality", "%s", fromtable(strquality, ival) ?: "");
	/* satvis */
	publish_topic("satvis", "%li", strtoul(nmea_safe_tok(NULL), NULL, 10));
	/* hdop */
	dval = nmea_strtod(nmea_safe_tok(NULL));
	if (!strcasestr(nmea_use, "GSA"))
		/* publish hdop from GGA only if GSA is not used */
		publish_topic("hdop", "%.1lf", dval);
	/* altitude */
	publish_topic("alt", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
	/* unknown */
	nmea_tok(NULL);
	/* geoidal seperation */
	publish_topic("geoid", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
}

static void recvd_gsa(void)
{
	int j, ival;

	/* selection mode */
	nmea_tok(NULL);
	/* gps mode (no fix, 2D, 3D) */
	ival = strtoul(nmea_safe_tok(NULL), NULL, 10);
	publish_topic("mode", "%s", fromtable(strmode, ival) ?: "");
	/* consume 12 satellites */
	for (j = 0; j < 12; ++j)
		nmea_tok(NULL);
	/* pdop, ... */
	publish_topic("pdop", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
	publish_topic("hdop", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
	publish_topic("vdop", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
}

static void recvd_gsv(void)
{
	__attribute__((unused))
	int isent, nsent;
	int prn, elv, azm, snr;
	int j;
	char *tok;
	static char topic[128];

	nsent = strtoul(nmea_safe_tok(NULL), NULL, 10);
	isent = strtoul(nmea_safe_tok(NULL), NULL, 10);
	nmea_tok(NULL); /* #sats in view */
	for (j = 0; j < 4; ++j) {
		tok = nmea_safe_tok(NULL);
		if (!strlen(tok))
			break;
		prn = strtoul(tok, NULL, 10);
		elv = strtoul(nmea_safe_tok(NULL), NULL, 10);
		azm = strtoul(nmea_safe_tok(NULL), NULL, 10);
		snr = strtoul(nmea_safe_tok(NULL), NULL, 10);

		/* publish satellite info non-retained.
		 * retained messages should be cleaned up,
		 * which implies that we must listen to our own sat info
		 * an remove 'lost' satellites ...
		 */
		sprintf(topic, "sat/%s:%i/elv", talker, prn);
		publish_topicr(topic, 0, "%i", elv);

		sprintf(topic, "sat/%s:%i/azm", talker, prn);
		publish_topicr(topic, 0, "%i", azm);

		sprintf(topic, "sat/%s:%i/snr", talker, prn);
		publish_topicr(topic, 0, "%i", snr);
	}
}

static void recvd_vtg(void)
{
	int j;

	/* true heading */
	publish_topic("heading", "%.2lf", nmea_strtod(nmea_safe_tok(NULL)));
	nmea_tok(NULL);
	/* magnetic heading */
	publish_topic("heading/magnetic", "%.2lf", nmea_strtod(nmea_safe_tok(NULL)));
	for (j = 4; j < 7; ++j)
		nmea_tok(NULL);
	publish_topic("speed", "%.2lf", nmea_strtod(nmea_safe_tok(NULL)));
}

static void recvd_zda(void)
{
	int val;
	time_t tim;
	struct tm tm = {};

	val = strtoul(nmea_safe_tok(NULL), NULL, 10);
	tm.tm_sec = val % 100; val /= 100;
	tm.tm_min = val % 100; val /= 100;
	tm.tm_hour = val;
	tm.tm_mday = strtoul(nmea_safe_tok(NULL), NULL, 10);
	tm.tm_mon  = strtoul(nmea_safe_tok(NULL), NULL, 10) - 1;
	tm.tm_year = strtoul(nmea_safe_tok(NULL), NULL, 10) - 1900;

	tim = timegm(&tm);
	publish_topic("utc", "%lu", tim);

	static char tstr[128];
	strftime(tstr, sizeof(tstr), "%a %d %b %Y %H:%M:%S", localtime(&tim));
	publish_topic("datetime", "%s", tstr);
}

static void recvd_line(char *line)
{
	char *tok;

	if (!*line)
		/* empty line */
		return;
	if (nmea_is_valid_sentence(line) < 0)
		return;
	tok = nmea_tok(line);
	if (strlen(tok) <= 2)
		/* bad line ? */
		return;
	in_data_sentence = 0;
	/* don't test the precise talker id */
	memcpy(talker, tok, 2);

	if (!strcasestr(nmea_use, tok+2))
		/* this sentence is blocked */
		goto done;
	else if (!strcmp(tok+2, "GGA"))
		recvd_gga();
	else if (!strcmp(tok+2, "GSA"))
		recvd_gsa();
	else if (!strcmp(tok+2, "GSV"))
		recvd_gsv();
	else if (!strcmp(tok+2, "VTG"))
		recvd_vtg();
	else if (!strcmp(tok+2, "ZDA"))
		recvd_zda();
	flush_pending_topics();
done:
	in_data_sentence = 0;
}

static char *lines;
static size_t linesize;
static size_t linepos;

static void recvd_lines(const char *line)
{
	int len = strlen(line);
	int mylen = strlen(lines ?: "");
	char *str;

	if (mylen + len + 1 > linesize) {
		/* grow */
		linesize = (mylen+len+1+1023) & ~1023;
		lines = realloc(lines, linesize);
		if (!lines)
			mylog(LOG_ERR, "realloc");
	}
	/* append */
	strcpy(lines+mylen, line);
	/* parse */
	for (;;) {
		str = strchr(lines+linepos, '\n');
		if (!str)
			break;
		if (str > lines+linepos && *(str-1) == '\r')
			/* cut \r too */
			*(str-1) = 0;
		/* null-terminate */
		*str++ = 0;
		recvd_line(lines+linepos);
		linepos = str-lines;
	}
	/* forget consumed data */
	if (linepos)
		memcpy(lines, lines+linepos, mylen+len-linepos+1);
	linepos = 0;
}

int main(int argc, char *argv[])
{
	int opt, ret;
	char *str;
	char mqtt_name[32];
	int logmask = LOG_UPTO(LOG_NOTICE);
	struct pollfd pf[3];

	setlocale(LC_ALL, "");
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
	case 'n':
		nmea_use = optarg;
		break;
	case 'p':
		topicprefix = optarg;
		break;
	case 'a':
		always = 1;
		break;
	case 'd':
		deaddelay = strtoul(optarg, NULL, 0);
		break;

	default:
		fprintf(stderr, "unknown option '%c'", opt);
	case '?':
		fputs(help_msg, stderr);
		exit(1);
		break;
	}

	atexit(my_exit);
	setlogmask(logmask);

	char *file;
	if (optind < argc) {
		/* extra file|device argument */
		file = argv[optind++];
		int fd;

		/* open file */
		fd = open(file, O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			mylog(LOG_ERR, "open %s: %s", file, ESTR(errno));
		/* set file|device as stdin */
		dup2(fd, STDIN_FILENO);
		close(fd);
	}

	if (mqtt_qos < 0)
		mqtt_qos = !strcmp(mqtt_host ?: "", "localhost") ? 0 : 1;
	/* MQTT start */
	mosquitto_lib_init();
	sprintf(mqtt_name, "%s-%i", NAME, getpid());
	mosq = mosquitto_new(mqtt_name, true, NULL);
	if (!mosq)
		mylog(LOG_ERR, "mosquitto_new failed: %s", ESTR(errno));
	/* mosquitto_will_set(mosq, "TOPIC", 0, NULL, mqtt_qos, 1); */

	ret = mosquitto_connect(mosq, mqtt_host, mqtt_port, mqtt_keepalive);
	if (ret)
		mylog(LOG_ERR, "mosquitto_connect %s:%i: %s", mqtt_host, mqtt_port, mosquitto_strerror(ret));
	mosquitto_message_callback_set(mosq, my_mqtt_msg);

	/* prepare signalfd */
	struct signalfd_siginfo sfdi;
	sigset_t sigmask;
	int sigfd;

	sigemptyset(&sigmask);
	sigfillset(&sigmask);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		mylog(LOG_ERR, "sigprocmask: %s", ESTR(errno));
	sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		mylog(LOG_ERR, "signalfd failed: %s", ESTR(errno));

	/* prepare poll */
	pf[0].fd = STDIN_FILENO;
	pf[0].events = POLL_IN;
	pf[1].fd = mosquitto_socket(mosq);
	pf[1].events = POLL_IN;
	pf[2].fd = sigfd;
	pf[2].events = POLL_IN;

	static char line[1024];
	/* schedule dead alarm */
	alarm(deaddelay);

	publish_topic("src", "%s", file ?: "-");
	while (!sigterm) {
		ret = poll(pf, 3, 1000);
		if (ret < 0)
			mylog(LOG_ERR, "poll ...");
		if (pf[0].revents) {
			/* read input events */
			ret = read(STDIN_FILENO, line, sizeof(line)-1);
			if (ret < 0 && errno == EAGAIN)
				/* another reader snooped our data away */
				goto gps_done;
			if (ret < 0)
				mylog(LOG_ERR, "read stdin: %s", ESTR(errno));
			/* schedule dead alarm */
			alarm(deaddelay);
			if (!ret)
				break;
			if (portalive < 1) {
				publish_topic("alive", "1");
				flush_pending_topics();
				portalive = 1;
			}

			line[ret] = 0;
			recvd_lines(line);
		}
gps_done:
		if (pf[1].revents) {
			/* mqtt read ... */
			ret = mosquitto_loop_read(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
		}
		while (pf[2].revents) {
			ret = read(sigfd, &sfdi, sizeof(sfdi));
			if (ret < 0 && errno == EAGAIN)
				break;
			if (ret < 0)
				mylog(LOG_ERR, "read signalfd: %s", ESTR(errno));
			switch (sfdi.ssi_signo) {
			case SIGTERM:
			case SIGINT:
				sigterm = 1;
				break;
			case SIGALRM:
				if (portalive != 0) {
					publish_topic("alive", "0");
					erase_topics(0);
					flush_pending_topics();
					portalive = 0;
				}
				/* schedule next */
				alarm(deaddelay);
				break;
			}
		}
		/* mosquitto things to do each iteration */
		ret = mosquitto_loop_misc(mosq);
		if (ret)
			mylog(LOG_ERR, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
		if (mosquitto_want_write(mosq)) {
			ret = mosquitto_loop_write(mosq, 1);
			if (ret)
				mylog(LOG_ERR, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
		}
	}

	erase_topics(1);
	/* terminate */
	send_self_sync(mosq);
	while (!ready) {
		ret = mosquitto_loop(mosq, 10, 1);
		if (ret < 0)
			mylog(LOG_ERR, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}

	return 0;
}
