// SPDX-License-Identifier: GPL-3.0-only
//
// See compat/pcap.h. Every entry point fails, because on Android there is no
// packet capture interface to open - frames arrive from devourer over USB.

#include "pcap.h"

#include <string.h>

static char g_error[] = "libpcap is not available on Android; frames come from devourer";

pcap_t *pcap_create(const char *source, char *errbuf) {
    (void)source;
    if (errbuf != NULL) {
        strncpy(errbuf, g_error, PCAP_ERRBUF_SIZE - 1);
        errbuf[PCAP_ERRBUF_SIZE - 1] = '\0';
    }
    return NULL;
}

int pcap_set_snaplen(pcap_t *p, int snaplen) { (void)p; (void)snaplen; return -1; }
int pcap_set_promisc(pcap_t *p, int promisc) { (void)p; (void)promisc; return -1; }
int pcap_set_timeout(pcap_t *p, int timeout_ms) { (void)p; (void)timeout_ms; return -1; }
int pcap_set_immediate_mode(pcap_t *p, int immediate) { (void)p; (void)immediate; return -1; }
int pcap_set_buffer_size(pcap_t *p, int size) { (void)p; (void)size; return -1; }
int pcap_activate(pcap_t *p) { (void)p; return -1; }

int pcap_setnonblock(pcap_t *p, int nonblock, char *errbuf) {
    (void)p;
    (void)nonblock;
    if (errbuf != NULL) {
        strncpy(errbuf, g_error, PCAP_ERRBUF_SIZE - 1);
        errbuf[PCAP_ERRBUF_SIZE - 1] = '\0';
    }
    return -1;
}

int pcap_datalink(pcap_t *p) { (void)p; return -1; }

int pcap_compile(pcap_t *p, struct bpf_program *fp, const char *str, int optimize,
                 bpf_u_int32 netmask) {
    (void)p; (void)fp; (void)str; (void)optimize; (void)netmask;
    return -1;
}

int pcap_setfilter(pcap_t *p, struct bpf_program *fp) { (void)p; (void)fp; return -1; }
void pcap_freecode(struct bpf_program *fp) { (void)fp; }
int pcap_get_selectable_fd(pcap_t *p) { (void)p; return -1; }
void pcap_close(pcap_t *p) { (void)p; }

const uint8_t *pcap_next(pcap_t *p, struct pcap_pkthdr *h) {
    (void)p;
    (void)h;
    return NULL;
}

char *pcap_geterr(pcap_t *p) { (void)p; return g_error; }
