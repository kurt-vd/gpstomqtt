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
#include <termios.h>
#include <mosquitto.h>
#include <sys/signalfd.h>
#include <sys/uio.h>

#define NAME "nmea0183tomqtt"
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

void mylog(int loglevel, const char *fmt, ...)
{
	va_list va;
	char *msg = NULL;

	if (logtostderr < 0)
		logtostderr = abs(isatty(STDERR_FILENO));

	if (logtostderr) {
		struct timespec tv;
		char timbuf[64];

		clock_gettime(CLOCK_REALTIME, &tv);
		strftime(timbuf, sizeof(timbuf), "%b %d %H:%M:%S", localtime(&tv.tv_sec));
		sprintf(timbuf+strlen(timbuf), ".%03u ", (int)(tv.tv_nsec/1000000));

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
	NAME ": Propagate nmea0183 input to MQTT\n"
	"usage:	" NAME " [OPTIONS ...] [FILE|DEVICE]\n"
	"\n"
	"Options\n"
	" -V, --version		Show version\n"
	" -v, --verbose		Be more verbose\n"
	" -h, --host=HOST[:PORT]Specify alternate MQTT host+port\n"
	" -n, --nmea=GGA[,ZDA...]	Specify what message to forward, absolute mode\n"
	" -n, --nmea=+/-GGA[,+/-ZDA...]	Specify what message to forward, relative mode\n"
	"		Possible messages are:\n"
	"		*GGA	lon, lat, alt, hdop, quality\n"
	"		 GNS	lon, lat, alt, hdop, quality for all talkers\n"
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
	" -d, --deadtime=DELAY	Consider port dead after DELAY seconds of silence (default 10)\n"
	" -D, --default=TK	Set a default talker (GP, GL, GB, GA, GN, ...)\n"
	"			The default talker's mode & dop will be published\n"
	"			without talker prefix for compatibility\n"
	"			Set to '0' to have no default talker\n"
	"\n"
	"Arguments\n"
	" FILE|DEVICE	Read input from FILE or DEVICE\n"
	"\n"
	"Runtime configuration via MQTT\n"
	" <PREFIX>/cfg/msgs	identical to --nmea parameter\n"
	" <PREFIX>/cfg/always	set --always parameter\n"
	" <PREFIX>/cfg/deadtime	set --deadtime parameter\n"
	" <PREFIX>/cfg/default	set --default parameter\n"
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
	{ "default", required_argument, NULL, 'D', },

	{ },
};
#else
#define getopt_long(argc, argv, optstring, longopts, longindex) \
	getopt((argc), (argv), (optstring))
#endif
static const char optstring[] = "Vv?h:n:p:ad:D:";

/* signal handler */
static volatile int sigterm;
static volatile int sigalrm;
static volatile int ready;

/* MQTT parameters */
static const char *mqtt_host = "localhost";
static int mqtt_port = 1883;
static int mqtt_keepalive = 10;
static int mqtt_qos = -1;

static char *file = "<stdin>";

/* state */
static struct mosquitto *mosq;

static char nmea_use[] = "+gga,-gns,-gsa,-gsv,+vtg,+zda\0\0";
static const char *def_talker = "gp";
static char *def_talker_mqtt;
static const char *topicprefix = "gps/";
static int topicprefixlen = 4;
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

/* message list api */
static int nmea_use_msg(const char *msg)
{
	char *str;

	str = strcasestr(nmea_use, msg);
	if (!str)
		return 0;
	return *(str-1) == '+';
}
static void merge_nmea_use(char *msgs)
{
	char *tok, *str;
	char mod;

	if (msgs[0] != '+' && msgs[0] != '-') {
		/* absolute mode, reset all */
		for (str = nmea_use; *str; str += 5)
			*str = '-';
	}
	for (tok = strtok(msgs, ","); tok; tok = strtok(NULL, ",")) {
		if (strchr("+-", tok[0]))
			mod = *tok++;
		else
			mod = '+';

		str = strcasestr(nmea_use, tok);
		if (str)
			*(str-1) = mod;
	}
}

static void clear_gsvs(void);
static void satuse_updated(const char *talker, int satuse);

/* MQTT API */
static char *myuuid;
static const char selfsynctopic[] = "tmp/selfsync";
static void send_self_sync(struct mosquitto *mosq)
{
	int ret;

	asprintf(&myuuid, "%i-%li-%i", getpid(), time(NULL), rand());

	ret = mosquitto_subscribe(mosq, NULL, selfsynctopic, mqtt_qos);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_subscribe %s: %s", selfsynctopic, mosquitto_strerror(ret));
	ret = mosquitto_publish(mosq, NULL, selfsynctopic, strlen(myuuid), myuuid, mqtt_qos, 0);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_publish %s: %s", selfsynctopic, mosquitto_strerror(ret));
}
static int is_self_sync(const struct mosquitto_message *msg)
{
	return !strcmp(msg->topic, selfsynctopic) &&
		!strncmp(myuuid ?: "", msg->payload ?: "", msg->payloadlen);
}

static void my_mqtt_msg(struct mosquitto *mosq, void *dat, const struct mosquitto_message *msg)
{
#define cfgprefix "cfg/"
#define cfgprefixlen 4

	if (is_self_sync(msg))
		ready = 1;

	if (!strncmp(msg->topic, topicprefix, topicprefixlen) &&
			!strncmp(msg->topic+topicprefixlen, cfgprefix, cfgprefixlen)) {
		const char *stopic = msg->topic + topicprefixlen + cfgprefixlen;

		if (!strcmp(stopic, "msgs")) {
			if (!msg->payloadlen)
				return;
			int gsv = nmea_use_msg("gsv");
			merge_nmea_use((char *)msg->payload);
			mylog(LOG_NOTICE, "nmea msgs changed to '%s'", nmea_use);
			if (gsv && !nmea_use_msg("gsv"))
				clear_gsvs();

		} else if (!strcmp(stopic, "always")) {
			always = strtoul((char *)msg->payload ?: "0", NULL, 0);
			mylog(LOG_NOTICE, "--%s changed to %u", stopic, always);

		} else if (!strcmp(stopic, "deadtime")) {
			deaddelay = strtoul((char *)msg->payload ?: "10", NULL, 0);
			mylog(LOG_NOTICE, "--%s changed to %u", stopic, deaddelay);

		} else if (!strcmp(stopic, "default")) {
			if (def_talker_mqtt)
				free(def_talker_mqtt);
			def_talker_mqtt = msg->payloadlen ? strdup(msg->payload) : NULL;
			mylog(LOG_NOTICE, "--%s changed to %s", stopic, def_talker_mqtt ?: def_talker);
		}
	}
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

__attribute__((format(printf,1,2)))
static const char *mktopic(const char *fmt, ...)
{
	va_list va;
	static char topic[1024];

	va_start(va, fmt);
	vsnprintf(topic, sizeof(topic), fmt, va);
	va_end(va);
	return topic;
}

#define FL_RETAIN		(1 << 0)
#define FL_IGN_DEF_TALKER	(1 << 1)
#define FL_NO_CACHE		(1 << 2)

#define publish_topic(topic, vfmt, ...) publish_topicrt(talker, (topic), FL_RETAIN, (vfmt), ##__VA_ARGS__)
#define publish_topicr(topic, flags, vfmt, ...) publish_topicrt(talker, (topic), (flags), (vfmt), ##__VA_ARGS__)

static void publish_cache(const char *realtopic, const char *value, int retain);
__attribute__((format(printf,4,5)))
static void publish_topicrt(const char *talker, const char *topic, int flags, const char *vfmt, ...)
{
	va_list va;
	static char value[1024];
	static char realtopic[1024];

	if (vfmt) {
		va_start(va, vfmt);
		vsprintf(value, vfmt, va);
		va_end(va);
	} else
		value[0] = 0;

	if (!strcmp(value, "nan"))
		strcpy(value, "");

	if (talker && ((flags & FL_IGN_DEF_TALKER) || strcmp(talker, def_talker_mqtt ?: def_talker)))
		sprintf(realtopic, "%s%s/%s", topicprefix, talker, topic);
	else
		sprintf(realtopic, "%s%s", topicprefix, topic);

	publish_cache(realtopic, value, flags);
}

static void publish_cache(const char *realtopic, const char *value, int flags)
{
	int ret;
	struct topic *it;

	if (!(flags & FL_RETAIN) || (flags & FL_NO_CACHE)) {
		ret = mosquitto_publish(mosq, NULL, realtopic, strlen(value), value, mqtt_qos, flags & FL_RETAIN);
		if (ret)
			mylog(LOG_ERR | LOG_EXIT, "mosquitto_publish %s: %s", realtopic, mosquitto_strerror(ret));
		return;
	}

	for (it = topics; it; it = it->next) {
		if (!strcmp(it->topic, realtopic))
			break;
	}
	if (!it) {
		it = malloc(sizeof(*it));
		if (!it)
			mylog(LOG_ERR | LOG_EXIT, "malloc failed: %s", ESTR(errno));
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
		it->retain = 1;
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
			if (ret)
				mylog(LOG_ERR | LOG_EXIT, "mosquitto_publish %s: %s", it->topic, mosquitto_strerror(ret));
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
		if (strchr(",", *str)) {
			*str++ = 0;
			break;
		}
	}
	saved_line = str;
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
			/* cut checksum field */
			*str = 0;
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

static void recvd_gga_gns(const char *msg)
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
	if (!strcasecmp(msg+2, "GGA")) {
		ival = strtoul(nmea_safe_tok(NULL), NULL, 10);
		publish_topic("quality", "%s", fromtable(strquality, ival) ?: "");
	} else {
		/* gns message */
		static const char gns_modes[] = "NADPRFEMS";
		static const char *const talkers[] = {
			"gp",
			"gl",
			"gb",
			"ga",
			NULL,
		};
		const char *const *talker;
		const char *chr;
		const char *tok;

		for (tok = nmea_safe_tok(NULL), talker = talkers;
				*talker && *tok; ++tok, ++talker) {
			chr = strchr(gns_modes, toupper(*tok));
			ival = chr ? chr - gns_modes : 0;
			publish_topicrt(*talker, "mode", 1,
					"%s", fromtable(strquality, ival) ?: "");
		}
	}
	/* sats-in-use */
	int satuse = strtoul(nmea_safe_tok(NULL), NULL, 10);
	publish_topicr("satuse", FL_RETAIN | FL_IGN_DEF_TALKER, "%i", satuse);
	satuse_updated(talker, satuse);
	/* hdop */
	dval = nmea_strtod(nmea_safe_tok(NULL));
	if (nmea_use_msg("GSA"))
		/* publish hdop from GGA only if GSA is not used */
		publish_topic("hdop", "%.1lf", dval);
	/* altitude */
	publish_topic("alt", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
	/* unknown */
	nmea_tok(NULL);
	/* geoidal seperation */
	publish_topic("geoid", "%.1lf", nmea_strtod(nmea_safe_tok(NULL)));
	if (!strcasecmp(msg+2, "GGA"))
		/* M for meters */
		nmea_tok(NULL);
	/* differential data
	 */
	publish_topic("diff/age", "%s", nmea_safe_tok(NULL));
	publish_topic("diff/id", "%s", nmea_safe_tok(NULL));
}

static void recvd_gsa(void)
{
	int j, ival, pktnr;
	double pdop, hdop, vdop;

	/* selection mode */
	nmea_tok(NULL);
	/* gps mode (no fix, 2D, 3D) */
	ival = strtoul(nmea_safe_tok(NULL), NULL, 10);
	/* consume 12 satellites */
	for (j = 0; j < 12; ++j)
		nmea_tok(NULL);
	/* pdop, ... */
	pdop = nmea_strtod(nmea_safe_tok(NULL));
	hdop = nmea_strtod(nmea_safe_tok(NULL));
	vdop = nmea_strtod(nmea_safe_tok(NULL));

	pktnr = strtoul(nmea_tok(NULL) ?: "1", NULL, 10);
	if (pktnr == 1) {
		/* only print on first packet */
		publish_topic("mode", "%s", fromtable(strmode, ival) ?: "");
		publish_topic("pdop", "%.1lf", pdop);
		publish_topic("hdop", "%.1lf", hdop);
		publish_topic("vdop", "%.1lf", vdop);
	}
}

/* GSV: keep track of satellites */
struct sat {
	int snr;
	int elv;
	int azm;
	int8_t recvd; /* recvd from NMEA */
	int8_t sent; /* sent to MQTT */
};
static struct sat *sats;
static int ssats;
/* range of sat. ids:
 * 1..32: GPS
 * 33..54: SBAS
 * 64..88/96: GLONASS
 * 193..195: QZSS
 * 201..235: Beidou
 * 301..336: Galileo
 */

struct gsv {
	char talker[3];
	int satmin, satmax;
	int satview;
	int sattrack, sattrack_saved;
	int satuse;
	int new;
	time_t trecvd;
};

static struct gsv *gsvs;
static int ngsvs, sgsvs;

static void clear_sat(const char *talker, int prn);

static struct gsv *find_gsv(const char *talker)
{
	struct gsv *gsv, *gsvend;

	gsvend = gsvs+ngsvs;
	for (gsv = gsvs; gsv < gsvend; ++gsv) {
		if (!strcmp(talker, gsv->talker))
			break;
	}
	if (gsv >= gsvend) {
		if (ngsvs >= sgsvs) {
			sgsvs += 16;
			gsvs = realloc(gsvs, sgsvs*sizeof(*gsvs));
			if (!gsvs)
				mylog(LOG_ERR | LOG_EXIT, "realloc %u gsvs: %s", sgsvs, ESTR(errno));
		}
		gsv = gsvs+ngsvs++;
		/* init new gsv struct */
		memset(gsv, 0, sizeof(*gsv));
		strcpy(gsv->talker, talker);
		gsv->new = 1;
	}
	return gsv;
}

static void satuse_updated(const char *talker, int satuse)
{
	struct gsv *gsv;
	int j, gn_satuse;
	static int gn_satuse_emitted;

	if (!strcmp(talker ?: "", "gn")) {
		gn_satuse_emitted = 1;
		/* ignore "GN", I'm aggregating it */
		return;
	}
	if (gn_satuse_emitted)
		/* device emit's satuse already */
		return;

	gsv = find_gsv(talker);

	if (always || gsv->satuse != satuse) {
		gsv->satuse = satuse;
		/* redo gn/satuse */
		gn_satuse = 0;
		for (j = 0; j < ngsvs; ++j)
			gn_satuse += gsvs[j].satuse;
		publish_topicrt("gn", "satuse", FL_RETAIN | FL_IGN_DEF_TALKER, "%i", gn_satuse);
	}
}

static void recvd_gsv(void)
{
	__attribute__((unused))
	int msgcnt, msgidx;
	int nsat;
	int prn, elv, azm, snr;
	int j;
	char *tok;
	struct gsv *gsv;
	struct sat *sat;

	gsv = find_gsv(talker);

	msgcnt = strtoul(nmea_safe_tok(NULL), NULL, 10);
	msgidx = strtoul(nmea_safe_tok(NULL), NULL, 10);
	nsat = strtoul(nmea_safe_tok(NULL), NULL, 10); /* #sats in view */

	gsv->trecvd = time(NULL);
	if (msgidx == 1) {
		/* start of block */
		for (j = gsv->satmin; j <= gsv->satmax && j < ssats; ++j)
			sats[j].recvd = 0;
		gsv->sattrack = 0;
	}

	for (j = 0; j < 4; ++j) {
		tok = nmea_safe_tok(NULL);
		if (!strlen(tok))
			break;
		prn = strtoul(tok, NULL, 10);
		elv = strtoul(nmea_safe_tok(NULL), NULL, 10);
		azm = strtoul(nmea_safe_tok(NULL), NULL, 10);
		snr = strtoul(nmea_tok(NULL) ?: "-1", NULL, 10);

		if (prn > ssats) {
			int oldssats = ssats;
			ssats = ssats ? ((prn + 127) & ~127) : 128;
			sats = realloc(sats, sizeof(*sats)*ssats);
			if (!sats)
				mylog(LOG_ERR | LOG_EXIT, "realloc %i sats: %s", ssats, ESTR(errno));
			memset(sats+oldssats, 0, sizeof(*sats)*(ssats - oldssats));
		}

		sat = sats+prn;

		/* publish satellite info non-retained.
		 * retained messages should be cleaned up,
		 * which implies that we must listen to our own sat info
		 * an remove 'lost' satellites ...
		 */
#define GSV_FLAGS	(FL_RETAIN | FL_NO_CACHE | FL_IGN_DEF_TALKER)
		if (always || !sat->sent || elv != sat->elv)
			publish_topicr(mktopic("sat/%i/elv", prn), GSV_FLAGS, "%i", elv);
		if (always || !sat->sent || azm != sat->azm)
			publish_topicr(mktopic("sat/%i/azm", prn), GSV_FLAGS, "%i", azm);
		if (always || !sat->sent || snr != sat->snr)
			publish_topicr(mktopic("sat/%i/snr", prn), GSV_FLAGS, (snr < 0) ? "" : "%i", snr);
		sat->elv = elv;
		sat->azm = azm;
		sat->snr = snr;
		sat->recvd = 1;
		sat->sent = 1;

		/* count nr. of really recvd sats */
		if (sat->snr >= 0)
			++gsv->sattrack;

		/* keep track of min/max prn of a talker */
		if (prn < gsv->satmin || !gsv->satmax)
			gsv->satmin = prn;
		if (prn > gsv->satmax)
			gsv->satmax = prn;
	}
	if (msgidx == msgcnt) {
		for (j = gsv->satmin; j < gsv->satmax; ++j)
			if (sats[j].sent && !sats[j].recvd)
				clear_sat(talker, j);
		/* emit number of sats in view
		 * not to confuse with 'satvis' which is actually 'satinuse'
		 * This can also act as a terminator of the satellite list
		 */
		if (always || gsv->new || nsat != gsv->satview)
			/* do not cache, it serves to terminate the block */
			publish_topicr("satview", FL_IGN_DEF_TALKER, "%i", nsat);
		gsv->satview = nsat;
		if (always || gsv->new || gsv->sattrack != gsv->sattrack_saved)
			publish_topicr("sattrack", FL_IGN_DEF_TALKER, "%i", gsv->sattrack);
		gsv->sattrack_saved = gsv->sattrack;
		gsv->new = 0;

		int satview = 0;
		int sattrack = 0;
		for (j = 0; j < ngsvs; ++j) {
			satview += gsvs[j].satview;
			sattrack += gsvs[j].sattrack_saved;
		}
		publish_topicrt("gn", "satview", FL_RETAIN | FL_IGN_DEF_TALKER, "%i", satview);
		publish_topicrt("gn", "sattrack", FL_RETAIN | FL_IGN_DEF_TALKER, "%i", sattrack);
	}
}

static void clear_sat(const char *talker, int prn)
{
	if (prn >= ssats)
		return;
	if (sats[prn].sent) {
		/* remove retained msgs */
		publish_topicrt(talker, mktopic("sat/%i/elv", prn), GSV_FLAGS, NULL);
		publish_topicrt(talker, mktopic("sat/%i/azm", prn), GSV_FLAGS, NULL);
		publish_topicrt(talker, mktopic("sat/%i/snr", prn), GSV_FLAGS, NULL);
	}
	memset(&sats[prn], 0, sizeof(sats[prn]));
}

static void clear_gsvs(void)
{
	int j, k;
	struct gsv *gsv;

	for (j = 0, gsv = gsvs; j < ngsvs; ++j, ++gsv) {
		for (k = gsv->satmin; k <= gsv->satmax; ++k)
			clear_sat(gsv->talker, k);
		publish_topicrt(gsv->talker, "satview", GSV_FLAGS, NULL);
		publish_topicrt(gsv->talker, "sattrack", GSV_FLAGS, NULL);
		gsv->satview = 0;
		gsv->sattrack = 0;
		gsv->sattrack_saved = 0;
	}
	ngsvs = 0;
	sgsvs = 0;
	ssats = 0;
	if (sats)
		free(sats);
	if (gsvs)
		free(gsvs);
}

static void recvd_txt(void)
{
	int level;
	const char *msg;
	static const int levels[256] = {
		[0] = LOG_ERR,
		[1] = LOG_WARNING,
		[2] = LOG_NOTICE,
		[7] = LOG_INFO,
	};

	nmea_tok(NULL);
	nmea_tok(NULL);

	level = strtoul(nmea_safe_tok(NULL), NULL, 10);
	msg = nmea_tok(NULL);

	if (levels[level] && msg)
		mylog(levels[level], "%s %c%cTXT '%s'", file, toupper(talker[0]), toupper(talker[1]), msg);
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
	talker[0] = tolower(tok[0]);
	talker[1] = tolower(tok[1]);

	if (!strcmp(tok+2, "TXT"))
		recvd_txt();
	else if (!nmea_use_msg(tok+2))
		/* this sentence is blocked */
		goto done;
	else if (!strcmp(tok+2, "GGA") || !strcmp(tok+2, "GNS"))
		recvd_gga_gns(tok);
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

/* ublox */
static uint16_t ublox_crc(const void *vdat, int len)
{
	const uint8_t *dat = vdat;
	uint8_t cka = 0, ckb = 0;

	for (; len; ++dat, --len) {
		cka += *dat;
		ckb += cka;
	}
	return (cka << 8) + ckb;
}

static void recvd_ublox_frame(const void *vdat, int len)
{
	const uint8_t *dat = vdat;
	uint16_t f_ck, my_ck;
	uint16_t clsid;

	my_ck = ublox_crc(dat+2, len-4);
	memcpy(&f_ck, dat+len-2, 2);
	f_ck = be16toh(f_ck);
	if (f_ck != my_ck) {
		mylog(LOG_WARNING, "ublox: crc mismatch");
		return;
	}
	memcpy(&clsid, dat+2, 2);
	/* class/id is formatted as BigEndian */
	clsid = be16toh(clsid);
	mylog(LOG_INFO, "ublox: %04x+%u", clsid, len-8);
}

/* multiplexer */
static char *buf;
static size_t buflen;
static size_t bufsize;

static void recvd_data(const char *line, int len)
{
	char *str;
	size_t bufpos;

	if (buflen + len + 1 > bufsize) {
		/* grow */
		bufsize = (buflen+len+1+1023) & ~1023;
		buf = realloc(buf, bufsize);
		if (!buf)
			mylog(LOG_ERR | LOG_EXIT, "realloc");
	}
	/* append */
	memcpy(buf+buflen, line, len);
	buflen += len;
	buf[buflen] = 0; /* null terminate */
	/* parse */
	for (bufpos = 0;;) {
		if (!memcmp(buf+bufpos, (uint8_t[]){ 0xb5, 0x62, }, 2)) {
			/* ublox header */
			uint16_t v16;

			if ((buflen - bufpos) < 8)
				/* incomplete empty ublox frame */
				break;
			memcpy(&v16, buf+bufpos+4, 2);
			v16 = le16toh(v16);
			v16 = buf[bufpos+4];
			if ((buflen - bufpos) < (v16+8))
				/* incomplete ublox frame */
				break;
			recvd_ublox_frame(buf+bufpos, v16+8);
			bufpos += v16+8;
			continue;
		}
		str = strchr(buf+bufpos, '\n');
		if (!str)
			break;
		if (str > buf+bufpos && *(str-1) == '\r')
			/* cut \r too */
			*(str-1) = 0;
		/* null-terminate */
		*str++ = 0;
		recvd_line(buf+bufpos);
		bufpos = str-buf;
	}
	/* forget consumed data */
	if (bufpos)
		memcpy(buf, buf+bufpos, buflen-bufpos+1);
	buflen -= bufpos;
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
		optarg = strdup(optarg);
		merge_nmea_use(optarg);
		free(optarg);
		break;
	case 'p':
		topicprefix = optarg;
		topicprefixlen = strlen(topicprefix);
		break;
	case 'a':
		always = 1;
		break;
	case 'd':
		deaddelay = strtoul(optarg, NULL, 0);
		break;
	case 'D':
		def_talker = optarg;
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

	if (optind < argc) {
		/* extra file|device argument */
		file = argv[optind++];
		int fd;
		struct termios term;

		/* open file */
		fd = open(file, O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0)
			mylog(LOG_ERR | LOG_EXIT, "open %s: %s", file, ESTR(errno));

		/* prepare port */
		if (tcgetattr(fd, &term) < 0) {
			if (errno != ENOTTY)
				mylog(LOG_ERR | LOG_EXIT, "tcgetattr %s: %s", file, ESTR(errno));
		} else {
			term.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | IXON | INLCR | IGNCR | ICRNL | INPCK);
			term.c_oflag &= ~(OPOST);
			term.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
			/* Replacing TCSAFLUSH by TCSANOW to avoid standard GPS blocked on some machines. */
			if (tcsetattr(fd, TCSANOW, &term) < 0)
				mylog(LOG_ERR | LOG_EXIT, "tcsetattr %s: %s", file, ESTR(errno));
		}
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

	asprintf(&str, "%s%s#", topicprefix, cfgprefix);
	ret = mosquitto_subscribe(mosq, NULL, str, mqtt_qos);
	if (ret)
		mylog(LOG_ERR | LOG_EXIT, "mosquitto_subscribe %s: %s", str, mosquitto_strerror(ret));
	free(str);

	/* prepare signalfd */
	struct signalfd_siginfo sfdi;
	sigset_t sigmask;
	int sigfd;

	sigemptyset(&sigmask);
	sigfillset(&sigmask);

	if (sigprocmask(SIG_BLOCK, &sigmask, NULL) < 0)
		mylog(LOG_ERR | LOG_EXIT, "sigprocmask: %s", ESTR(errno));
	sigfd = signalfd(-1, &sigmask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		mylog(LOG_ERR | LOG_EXIT, "signalfd failed: %s", ESTR(errno));

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

	publish_topicrt(NULL, "src", 1, "%s", file ?: "-");
	while (!sigterm) {
		ret = poll(pf, 3, 1000);
		if (ret < 0)
			mylog(LOG_ERR | LOG_EXIT, "poll ...");
		if (pf[0].revents) {
			/* read input events */
			ret = read(STDIN_FILENO, line, sizeof(line));
			if (ret < 0 && errno == EAGAIN)
				/* another reader snooped our data away */
				goto gps_done;
			if (ret < 0)
				mylog(LOG_ERR | LOG_EXIT, "read stdin: %s", ESTR(errno));
			/* schedule dead alarm */
			alarm(deaddelay);
			if (!ret)
				break;
			if (portalive < 1) {
				publish_topicrt(NULL, "alive", 1, "1");
				flush_pending_topics();
				portalive = 1;
			}
			recvd_data(line, ret);
		}
gps_done:
		if (pf[1].revents) {
			/* mqtt read ... */
			ret = mosquitto_loop_read(mosq, 1);
			if (ret)
				mylog(LOG_ERR | LOG_EXIT, "mosquitto_loop_read: %s", mosquitto_strerror(ret));
		}
		while (pf[2].revents) {
			ret = read(sigfd, &sfdi, sizeof(sfdi));
			if (ret < 0 && errno == EAGAIN)
				break;
			if (ret < 0)
				mylog(LOG_ERR | LOG_EXIT, "read signalfd: %s", ESTR(errno));
			switch (sfdi.ssi_signo) {
			case SIGTERM:
			case SIGINT:
				sigterm = 1;
				break;
			case SIGALRM:
				if (portalive != 0) {
					publish_topicrt(NULL, "alive", 1, "0");
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
			mylog(LOG_ERR | LOG_EXIT, "mosquitto_loop_misc: %s", mosquitto_strerror(ret));
		if (mosquitto_want_write(mosq)) {
			ret = mosquitto_loop_write(mosq, 1);
			if (ret)
				mylog(LOG_ERR | LOG_EXIT, "mosquitto_loop_write: %s", mosquitto_strerror(ret));
		}
	}

	erase_topics(1);
	clear_gsvs();
	/* terminate */
	send_self_sync(mosq);
	while (!ready) {
		ret = mosquitto_loop(mosq, 10, 1);
		if (ret)
			mylog(LOG_ERR | LOG_EXIT, "mosquitto_loop: %s", mosquitto_strerror(ret));
	}

	return 0;
}
