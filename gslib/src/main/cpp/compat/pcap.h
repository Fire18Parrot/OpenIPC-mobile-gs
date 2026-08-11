// SPDX-License-Identifier: GPL-3.0-only
//
// A stand-in for libpcap, sufficient to compile wfb-ng's rx.cpp on Android.
//
// rx.cpp contains two independent things: the Aggregator, which is what we
// want, and a pcap-based Receiver that reads frames off a monitor-mode
// interface. On a phone there is no monitor interface and no kernel driver -
// devourer hands us frames directly - so the Receiver is never constructed.
// Because both live in one translation unit the linker still needs the pcap
// symbols to resolve, which pcap_stub.c provides as hard failures.
//
// If a Receiver is ever constructed by accident it throws immediately rather
// than silently doing nothing.

#pragma once

#include <stdint.h>
#include <sys/time.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCAP_ERRBUF_SIZE 256
#define DLT_IEEE802_11_RADIO 127

typedef struct pcap pcap_t;

struct pcap_pkthdr {
    struct timeval ts;
    uint32_t caplen;
    uint32_t len;
};

struct bpf_insn;

struct bpf_program {
    unsigned int bf_len;
    struct bpf_insn *bf_insns;
};

typedef int bpf_u_int32;

pcap_t *pcap_create(const char *source, char *errbuf);
int pcap_set_snaplen(pcap_t *p, int snaplen);
int pcap_set_promisc(pcap_t *p, int promisc);
int pcap_set_timeout(pcap_t *p, int timeout_ms);
int pcap_set_immediate_mode(pcap_t *p, int immediate);
int pcap_set_buffer_size(pcap_t *p, int size);
int pcap_activate(pcap_t *p);
int pcap_setnonblock(pcap_t *p, int nonblock, char *errbuf);
int pcap_datalink(pcap_t *p);
int pcap_compile(pcap_t *p, struct bpf_program *fp, const char *str, int optimize,
                 bpf_u_int32 netmask);
int pcap_setfilter(pcap_t *p, struct bpf_program *fp);
void pcap_freecode(struct bpf_program *fp);
int pcap_get_selectable_fd(pcap_t *p);
void pcap_close(pcap_t *p);
const uint8_t *pcap_next(pcap_t *p, struct pcap_pkthdr *h);
char *pcap_geterr(pcap_t *p);

#ifdef __cplusplus
}
#endif
