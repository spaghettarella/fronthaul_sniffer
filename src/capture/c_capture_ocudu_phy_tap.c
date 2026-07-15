#include "c_capture_ocudu_phy_tap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
	FH_PHY_TAP_STATE_MAGIC = 0x52534250u,
	FH_PHY_TAP_RECORD_MAGIC = 0x50595450u,
	FH_PHY_TAP_RECORD_VERSION = 2,
	FH_PHY_TAP_PACKET_MAGIC = 0x46505450u,
	FH_PHY_TAP_PACKET_VERSION = 2,
};

typedef struct fh_phy_tap_state_header {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint32_t capacity;
	uint32_t sample_count;
	uint64_t write_sequence;
	uint64_t read_sequence;
	uint64_t dropped_count;
	uint32_t init_done;
} fh_phy_tap_state_header_t;

typedef struct fh_phy_tap_record_header {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint32_t eaxc_id;
	uint32_t frame_id;
	uint32_t subframe_id;
	uint32_t slot_id;
	uint32_t symbol_id;
	uint32_t numerology;
	uint32_t stream_index;
	uint32_t sample_count;
	/// O-RAN data direction of the tapped samples. 0 = uplink, 1 = downlink (matches ocudu::ofh::data_direction).
	uint8_t data_direction;
	/// Bit width of each I or Q component in the payload.
	uint8_t iq_bit_width;
	/// IQ compression method of the payload (matches ocudu::ofh::compression_type). 0 = none.
	uint8_t compression_method;
	uint8_t reserved2;
	/// First physical resource block (PRB) index covered by this record.
	uint32_t start_prbu;
	/// Number of PRBs covered by this record.
	uint32_t num_prbu;
	uint64_t sequence;
	uint64_t timestamp_ns;
} fh_phy_tap_record_header_t;

typedef struct fh_phy_tap_packet_header {
	uint32_t magic;
	uint16_t version;
	uint16_t reserved;
	uint32_t eaxc_id;
	uint32_t frame_id;
	uint32_t subframe_id;
	uint32_t slot_id;
	uint32_t symbol_id;
	uint32_t numerology;
	uint32_t stream_index;
	uint32_t sample_count;
	uint32_t start_prbu;
	uint32_t num_prbu;
	uint8_t data_direction;
	uint8_t iq_bit_width;
	uint8_t compression_method;
	uint8_t reserved2;
	uint64_t timestamp_ns;
} __attribute__((packed)) fh_phy_tap_packet_header_t;

typedef struct fh_mapped_queue {
	int fd;
	size_t mapped_bytes;
	void* mapping;
} fh_mapped_queue_t;

static int write_err(char* errbuf, size_t errbuf_len, const char* msg) {
	if (errbuf != NULL && errbuf_len > 0) {
		(void)snprintf(errbuf, errbuf_len, "%s", msg);
	}
	return -1;
}

static void normalize_shared_memory_name(const char* raw, char* out, size_t out_len) {
	if (out == NULL || out_len == 0) {
		return;
	}

	const char* source = raw;
	if (source == NULL || source[0] == '\0') {
		source = "/ocudu_phy_tap_queue";
	}

	size_t out_pos = 0;
	if (source[0] != '/' && out_pos + 1 < out_len) {
		out[out_pos++] = '/';
	}

	for (size_t i = 0; source[i] != '\0' && out_pos + 1 < out_len; ++i) {
		char ch = source[i];
		if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
			ch = '_';
		}
		out[out_pos++] = ch;
	}
	out[out_pos] = '\0';
}

static size_t round_up(size_t value, size_t alignment) {
	if (alignment == 0) {
		return value;
	}
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int map_queue(const char* queue_name, fh_mapped_queue_t* out, char* errbuf, size_t errbuf_len) {
	if (queue_name == NULL || out == NULL) {
		return write_err(errbuf, errbuf_len, "invalid queue mapping arguments");
	}

	memset(out, 0, sizeof(*out));
	out->fd = -1;

	out->fd = shm_open(queue_name, O_RDWR, 0600);
	if (out->fd < 0) {
		return write_err(errbuf, errbuf_len, "failed to open shared memory queue");
	}

	struct stat st;
	memset(&st, 0, sizeof(st));
	if (fstat(out->fd, &st) != 0) {
		close(out->fd);
		out->fd = -1;
		return write_err(errbuf, errbuf_len, "failed to stat shared memory queue");
	}

	out->mapped_bytes = (size_t)st.st_size;
	out->mapping = mmap(NULL, out->mapped_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
	if (out->mapping == MAP_FAILED) {
		close(out->fd);
		out->fd = -1;
		out->mapping = NULL;
		return write_err(errbuf, errbuf_len, "failed to map shared memory queue");
	}

	return 0;
}

static void unmap_queue(fh_mapped_queue_t* mapped) {
	if (mapped == NULL) {
		return;
	}
	if (mapped->mapping != NULL && mapped->mapping != MAP_FAILED) {
		munmap(mapped->mapping, mapped->mapped_bytes);
	}
	if (mapped->fd >= 0) {
		close(mapped->fd);
	}
	mapped->mapping = NULL;
	mapped->fd = -1;
	mapped->mapped_bytes = 0;
}

int fh_capture_ocudu_phy_tap_run(
	const fh_capture_ocudu_phy_tap_options_t* options,
	fh_packet_callback_t on_packet,
	void* user_ctx,
	const volatile sig_atomic_t* stop_flag,
	char* errbuf,
	size_t errbuf_len) {
	if (options == NULL || on_packet == NULL || stop_flag == NULL) {
		return write_err(errbuf, errbuf_len, "invalid ocudu phy tap capture arguments");
	}

	char queue_name[256];
	normalize_shared_memory_name(options->queue_name, queue_name, sizeof(queue_name));

	const int poll_sleep_ms = (options->poll_sleep_ms <= 0) ? 1 : options->poll_sleep_ms;

	fh_mapped_queue_t mapped;
	if (map_queue(queue_name, &mapped, errbuf, errbuf_len) != 0) {
		return -1;
	}

	int rc = 0;
	fh_phy_tap_state_header_t* state = (fh_phy_tap_state_header_t*)mapped.mapping;
	if (state == NULL || state->magic != FH_PHY_TAP_STATE_MAGIC || state->version != 1) {
		unmap_queue(&mapped);
		return write_err(errbuf, errbuf_len, "shared queue layout mismatch or uninitialized queue");
	}

	if (state->capacity == 0 || state->sample_count == 0) {
		unmap_queue(&mapped);
		return write_err(errbuf, errbuf_len, "invalid shared queue dimensions");
	}

	const size_t iq_bytes = (size_t)state->sample_count * sizeof(uint16_t) * 2U;
	const size_t record_stride = round_up(sizeof(fh_phy_tap_record_header_t) + iq_bytes, sizeof(max_align_t));
	const uint8_t* records_base = (const uint8_t*)mapped.mapping + sizeof(fh_phy_tap_state_header_t);

	const size_t packet_bytes = sizeof(fh_phy_tap_packet_header_t) + iq_bytes;
	uint8_t* packet_storage = (uint8_t*)malloc(packet_bytes);
	if (packet_storage == NULL) {
		unmap_queue(&mapped);
		return write_err(errbuf, errbuf_len, "failed to allocate phy tap packet buffer");
	}

	while (*stop_flag == 0) {
		const uint64_t read_seq = __atomic_load_n(&state->read_sequence, __ATOMIC_ACQUIRE);
		const uint64_t write_seq = __atomic_load_n(&state->write_sequence, __ATOMIC_ACQUIRE);

		if (read_seq == write_seq) {
			struct timespec delay;
			delay.tv_sec = poll_sleep_ms / 1000;
			delay.tv_nsec = (long)(poll_sleep_ms % 1000) * 1000L * 1000L;
			nanosleep(&delay, NULL);
			continue;
		}

		const size_t slot_index = (size_t)(read_seq % (uint64_t)state->capacity);
		const uint8_t* slot_address = records_base + slot_index * record_stride;
		const fh_phy_tap_record_header_t* record = (const fh_phy_tap_record_header_t*)slot_address;

		if (record->magic != FH_PHY_TAP_RECORD_MAGIC || record->version != FH_PHY_TAP_RECORD_VERSION ||
			record->sample_count != state->sample_count || record->sequence != read_seq) {
			struct timespec delay;
			delay.tv_sec = 0;
			delay.tv_nsec = 1000L * 1000L;
			nanosleep(&delay, NULL);
			continue;
		}

		fh_phy_tap_packet_header_t* out_hdr = (fh_phy_tap_packet_header_t*)packet_storage;
		memset(out_hdr, 0, sizeof(*out_hdr));
		out_hdr->magic = FH_PHY_TAP_PACKET_MAGIC;
		out_hdr->version = FH_PHY_TAP_PACKET_VERSION;
		out_hdr->eaxc_id = record->eaxc_id;
		out_hdr->frame_id = record->frame_id;
		out_hdr->subframe_id = record->subframe_id;
		out_hdr->slot_id = record->slot_id;
		out_hdr->symbol_id = record->symbol_id;
		out_hdr->numerology = record->numerology;
		out_hdr->stream_index = record->stream_index;
		out_hdr->sample_count = record->sample_count;
		out_hdr->start_prbu = record->start_prbu;
		out_hdr->num_prbu = record->num_prbu;
		if (out_hdr->num_prbu == 0) {
			out_hdr->num_prbu = (record->sample_count + 11U) / 12U;
			if (out_hdr->num_prbu == 0) {
				out_hdr->num_prbu = 1;
			}
		}
		if (out_hdr->num_prbu > 273U) {
			out_hdr->num_prbu = 273U;
		}
		out_hdr->data_direction = record->data_direction;
		out_hdr->iq_bit_width = record->iq_bit_width;
		out_hdr->compression_method = record->compression_method;
		out_hdr->timestamp_ns = record->timestamp_ns;

		memcpy(packet_storage + sizeof(*out_hdr), slot_address + sizeof(*record), iq_bytes);

		fh_packet_t pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.data = packet_storage;
		pkt.caplen = packet_bytes;
		pkt.len = packet_bytes;
		pkt.timestamp.tv_sec = (time_t)(record->timestamp_ns / 1000000000ULL);
		pkt.timestamp.tv_nsec = (long)(record->timestamp_ns % 1000000000ULL);
		(void)snprintf(pkt.ingress_if, sizeof(pkt.ingress_if), "ocudu_phy_tap");

		if (on_packet(&pkt, user_ctx) != 0) {
			break;
		}

		__atomic_store_n(&state->read_sequence, read_seq + 1ULL, __ATOMIC_RELEASE);
	}

	free(packet_storage);
	unmap_queue(&mapped);
	return rc;
}
