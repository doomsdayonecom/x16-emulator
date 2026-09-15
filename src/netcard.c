// Commander X16 Emulator — a virtual serial/network card. See netcard.h.
//
// THE MODEM'S SIDE OF THE CONVERSATION, as the X16 floor of C*'s CLM expects
// it, because that floor is what this exists to test:
//
//   +++            in data mode, back to command mode with the call kept
//   ATH0<CR>       hang up; OK if there was a call, ERROR if not
//   ATD"host:port"<CR>   dial; CONNECT, or NO CARRIER when nothing answers
//   ATI<CR>        a Zimodem-shaped report: CONNECTED TO <ssid> (<address>)
//   ATE0/ATE1      echo off/on (on, as Zimodem ships)
//   AT<CR>         OK
//
// Everything else is answered ERROR, which is also what Zimodem does.
// Commands are echoed while echo is on, as the card's firmware does, so a
// program that has to skip its own echo gets to practise here.

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L   // getaddrinfo under -std=c11
#define _DEFAULT_SOURCE
#endif

#include "netcard.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define NETCARD_SOCKETS 1
#else
#define NETCARD_SOCKETS 0
#endif

bool has_netcard = false;
uint16_t netcard_addr = 0x9f60;

// 16450 registers
#define LCR_DLAB   0x80
#define LSR_DR     0x01
#define LSR_OE     0x02
#define LSR_THRE   0x20
#define LSR_TEMT   0x40
#define MSR_CTS    0x10
#define MSR_DSR    0x20
#define MSR_DCD    0x80

#define RX_CAP 4096
#define LINE_CAP 256

static struct {
	uint8_t dll, dlm, ier, lcr, mcr, scr, fcr;
	uint8_t rx[RX_CAP];
	int rx_head, rx_tail;
	bool overrun;
	// the modem
	bool echo;
	bool online;            // a call is up (the socket is open)
	bool data_mode;         // bytes go to the wire rather than the parser
	char line[LINE_CAP];
	int line_len;
	int plus_count;         // +++ seen in data mode
	int fd;
	bool connecting;
	char host[128];
	int port;
} card;

static void rx_put(uint8_t b)
{
	int next = (card.rx_tail + 1) % RX_CAP;
	if (next == card.rx_head) {
		card.overrun = true;
		return;
	}
	card.rx[card.rx_tail] = b;
	card.rx_tail = next;
}

static void rx_str(const char *s)
{
	while (*s) {
		rx_put((uint8_t)*s++);
	}
}

static void reply(const char *s)
{
	rx_str("\r\n");
	rx_str(s);
	rx_str("\r\n");
}

static void hangup(void)
{
#if NETCARD_SOCKETS
	if (card.fd >= 0) {
		close(card.fd);
	}
#endif
	card.fd = -1;
	card.online = false;
	card.connecting = false;
	card.data_mode = false;
}

void netcard_init(void)
{
	memset(&card, 0, sizeof card);
	card.fd = -1;
	card.echo = true;
	card.lcr = 0x03;
}

// ATD"host:port" — begin a connection; the answer comes from netcard_step.
static void dial(const char *arg)
{
	char buf[160];
	char *colon;
	size_t n;

	while (*arg == '"' || *arg == 'T' || *arg == 'P' || *arg == ' ') {
		arg++;
	}
	n = strlen(arg);
	if (n >= sizeof buf) {
		reply("ERROR");
		return;
	}
	memcpy(buf, arg, n + 1);
	while (n > 0 && (buf[n - 1] == '"' || buf[n - 1] == ' ')) {
		buf[--n] = '\0';
	}
	colon = strrchr(buf, ':');
	if (!colon || colon == buf) {
		reply("ERROR");
		return;
	}
	*colon = '\0';
	snprintf(card.host, sizeof card.host, "%s", buf);
	card.port = atoi(colon + 1);
	if (card.port <= 0 || card.port > 65535) {
		reply("ERROR");
		return;
	}

#if NETCARD_SOCKETS
	{
		struct addrinfo hints, *res = NULL;
		char portstr[8];
		snprintf(portstr, sizeof portstr, "%d", card.port);
		memset(&hints, 0, sizeof hints);
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(card.host, portstr, &hints, &res) != 0 || !res) {
			reply("NO CARRIER");
			return;
		}
		card.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (card.fd < 0) {
			freeaddrinfo(res);
			reply("NO CARRIER");
			return;
		}
		fcntl(card.fd, F_SETFL, fcntl(card.fd, F_GETFL, 0) | O_NONBLOCK);
		{
			int one = 1;
			(void)setsockopt(card.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
		}
		if (connect(card.fd, res->ai_addr, res->ai_addrlen) < 0 && errno != EINPROGRESS) {
			freeaddrinfo(res);
			hangup();
			reply("NO CARRIER");
			return;
		}
		freeaddrinfo(res);
		card.connecting = true;
	}
#else
	reply("NO CARRIER");
#endif
}

// One AT command line, without its CR.
static void command(char *cmd)
{
	char *p = cmd;
	while (*p == '+') {           // a +++ that arrived in command mode is nothing
		p++;
	}
	while (*p == ' ') {
		p++;
	}
	if (*p == '\0') {
		return;
	}
	if (toupper((unsigned char)p[0]) != 'A' || toupper((unsigned char)p[1]) != 'T') {
		reply("ERROR");
		return;
	}
	p += 2;
	if (*p == '\0') {
		reply("OK");
		return;
	}
	switch (toupper((unsigned char)*p)) {
		case 'H':
			if (card.online || card.connecting) {
				hangup();
				reply("OK");
			} else {
				reply("ERROR");
			}
			return;
		case 'D':
			if (card.online || card.connecting) {
				reply("ERROR");
				return;
			}
			dial(p + 1);
			return;
		case 'E':
			card.echo = (p[1] != '0');
			reply("OK");
			return;
		case 'I':
			rx_str("\r\nZimodem-ish 1.0 (x16emu virtual card)\r\n");
			rx_str("CONNECTED TO x16emu (127.0.0.1)\r\n");
			reply("OK");
			return;
		case 'O':
			if (card.online) {
				card.data_mode = true;
				reply("CONNECT");
			} else {
				reply("ERROR");
			}
			return;
		case 'Z':
			hangup();
			card.echo = true;
			reply("OK");
			return;
		default:
			reply("ERROR");
			return;
	}
}

static void tx_byte(uint8_t b)
{
	if (card.data_mode) {
		// +++ with nothing else around it escapes to command mode. The guard
		// time is not modelled; three plusses in a row are enough here.
		if (b == '+') {
			if (++card.plus_count == 3) {
				card.plus_count = 0;
				card.data_mode = false;
				reply("OK");
			}
			return;
		}
		if (card.plus_count) {
			// they were data after all
#if NETCARD_SOCKETS
			if (card.fd >= 0) {
				const char plus[3] = {'+', '+', '+'};
				(void)!write(card.fd, plus, card.plus_count);
			}
#endif
			card.plus_count = 0;
		}
#if NETCARD_SOCKETS
		if (card.fd >= 0) {
			(void)!write(card.fd, &b, 1);
		}
#endif
		return;
	}
	if (card.echo) {
		rx_put(b);
	}
	if (b == '\r' || b == '\n') {
		if (card.line_len > 0) {
			card.line[card.line_len] = '\0';
			card.line_len = 0;
			command(card.line);
		}
		return;
	}
	if (b == 8 || b == 127) {
		if (card.line_len > 0) {
			card.line_len--;
		}
		return;
	}
	if (card.line_len < LINE_CAP - 1) {
		card.line[card.line_len++] = (char)b;
	}
}

// Called once a frame from the main loop: finish a connection in progress,
// and move bytes from the wire into the receive FIFO.
void netcard_step(void)
{
#if NETCARD_SOCKETS
	if (card.connecting) {
		fd_set w;
		struct timeval tv = {0, 0};
		FD_ZERO(&w);
		FD_SET(card.fd, &w);
		if (select(card.fd + 1, NULL, &w, NULL, &tv) > 0) {
			int err = 0;
			socklen_t len = sizeof err;
			(void)getsockopt(card.fd, SOL_SOCKET, SO_ERROR, &err, &len);
			card.connecting = false;
			if (err != 0) {
				hangup();
				reply("NO CARRIER");
			} else {
				card.online = true;
				card.data_mode = true;
				card.plus_count = 0;
				reply("CONNECT");
			}
		}
		return;
	}
	if (card.online && card.fd >= 0) {
		uint8_t buf[512];
		for (;;) {
			ssize_t n = read(card.fd, buf, sizeof buf);
			if (n > 0) {
				for (ssize_t i = 0; i < n; i++) {
					rx_put(buf[i]);
				}
				if (n < (ssize_t)sizeof buf) {
					break;
				}
			} else if (n == 0) {
				hangup();
				reply("NO CARRIER");
				break;
			} else {
				break;    // EAGAIN: nothing waiting
			}
		}
	}
#endif
}

uint8_t netcard_read(uint8_t reg, bool debugOn)
{
	if (reg >= 8) {
		return 0x9f;    // the DE-9 half: not here
	}
	switch (reg) {
		case 0:
			if (card.lcr & LCR_DLAB) {
				return card.dll;
			}
			if (card.rx_head == card.rx_tail) {
				return 0;
			}
			{
				uint8_t b = card.rx[card.rx_head];
				if (!debugOn) {
					card.rx_head = (card.rx_head + 1) % RX_CAP;
				}
				return b;
			}
		case 1:
			return (card.lcr & LCR_DLAB) ? card.dlm : card.ier;
		case 2:
			return 0x01 | (card.fcr & 0x01 ? 0xc0 : 0);   // no interrupt pending
		case 3:
			return card.lcr;
		case 4:
			return card.mcr & 0x3f;
		case 5: {
			uint8_t lsr = LSR_THRE | LSR_TEMT;
			if (card.rx_head != card.rx_tail) {
				lsr |= LSR_DR;
			}
			if (card.overrun) {
				lsr |= LSR_OE;
				if (!debugOn) {
					card.overrun = false;
				}
			}
			return lsr;
		}
		case 6: {
			uint8_t msr = MSR_CTS | MSR_DSR;
			if (card.online) {
				msr |= MSR_DCD;
			}
			return msr;
		}
		case 7:
			return card.scr;
	}
	return 0x9f;
}

void netcard_write(uint8_t reg, uint8_t val)
{
	if (reg >= 8) {
		return;
	}
	switch (reg) {
		case 0:
			if (card.lcr & LCR_DLAB) {
				card.dll = val;
			} else {
				tx_byte(val);
			}
			break;
		case 1:
			if (card.lcr & LCR_DLAB) {
				card.dlm = val;
			} else {
				card.ier = val & 0x0f;
			}
			break;
		case 2:
			card.fcr = val;
			if (val & 0x02) {    // clear the receive FIFO
				card.rx_head = card.rx_tail = 0;
			}
			break;
		case 3:
			card.lcr = val;
			break;
		case 4:
			card.mcr = val;
			break;
		case 7:
			card.scr = val;
			break;
		default:
			break;
	}
}
