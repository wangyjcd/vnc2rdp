#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.h"
#include "rdp.h"
#include "vnc.h"
#include "vncauth.h"

static int
r2v_vnc_recv(r2v_vnc_t *v)
{
	int n = 0;

	r2v_packet_reset(v->packet);

	n = recv(v->fd, v->packet->current, v->packet->max_len, 0);
	if (n == -1 || n == 0) {
		goto fail;
	}
	v->packet->end += n;

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_recv1(r2v_vnc_t *v, size_t len)
{
	int n = 0;

	r2v_packet_reset(v->packet);

	n = recv(v->fd, v->packet->current, len, MSG_WAITALL);
	if (n == -1 || n == 0) {
		goto fail;
	}
	v->packet->end += n;

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_send(r2v_vnc_t *v)
{
	int n = 0;

	n = send(v->fd, v->packet->data, R2V_PACKET_LEN(v->packet), 0);
	if (n == -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_process_vnc_authentication(r2v_vnc_t *v)
{
	uint8_t challenge[CHALLENGESIZE];
	uint32_t security_result;

	/* receive challenge */
	if (r2v_vnc_recv1(v, CHALLENGESIZE) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_N(v->packet, challenge, CHALLENGESIZE);
	rfbEncryptBytes(challenge, v->password);
	/* send response */
	r2v_packet_reset(v->packet);
	R2V_PACKET_WRITE_N(v->packet, challenge, CHALLENGESIZE);
	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}
	/* receive SecurityResult */
	if (r2v_vnc_recv1(v, sizeof(security_result)) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_UINT32_BE(v->packet, security_result);
	if (security_result == RFB_SEC_RESULT_OK) {
		r2v_log_info("vnc authentication success");
	} else {
		r2v_log_error("vnc authentication failed");
		goto fail;
	}

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_build_conn(r2v_vnc_t *v)
{
	/* receive ProtocolVersion */
	if (r2v_vnc_recv(v) == -1) {
		goto fail;
	}

	/* send ProtocolVersion */
	r2v_packet_reset(v->packet);
	R2V_PACKET_WRITE_N(v->packet, RFB_PROTOCOL_VERSION,
					   strlen(RFB_PROTOCOL_VERSION));
	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}

	/* receive security-type */
	if (r2v_vnc_recv1(v, sizeof(v->security_type)) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_UINT32_BE(v->packet, v->security_type);
	switch (v->security_type) {
	case RFB_SEC_TYPE_NONE:
		break;
	case RFB_SEC_TYPE_VNC_AUTH:
		if (r2v_vnc_process_vnc_authentication(v) == -1) {
			goto fail;
		}
		break;
	default:
		goto fail;
	}

	/* send ClientInit message */
	r2v_packet_reset(v->packet);
	R2V_PACKET_WRITE_UINT8(v->packet, 1);
	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}

	/* recv ServerInit message */
	if (r2v_vnc_recv(v) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_UINT16_BE(v->packet, v->framebuffer_width);
	R2V_PACKET_READ_UINT16_BE(v->packet, v->framebuffer_height);
	R2V_PACKET_READ_UINT8(v->packet, v->bits_per_pixel);
	R2V_PACKET_READ_UINT8(v->packet, v->depth);
	R2V_PACKET_READ_UINT8(v->packet, v->big_endian_flag);
	R2V_PACKET_READ_UINT8(v->packet, v->true_colour_flag);
	r2v_log_info("server framebuffer size: %dx%d", v->framebuffer_width,
				 v->framebuffer_height);
	r2v_log_info("server bits_per_pixel: %d, depth: %d, big_endian_flag: %d, "
				 "true_colour_flag: %d", v->bits_per_pixel, v->depth,
				 v->big_endian_flag, v->true_colour_flag);

	/* send SetPixelFormat message */
	r2v_packet_reset(v->packet);
	R2V_PACKET_WRITE_UINT8(v->packet, RFB_SET_PIXEL_FORMAT);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	/* bits-per-pixel */
	R2V_PACKET_WRITE_UINT8(v->packet, 32);
	/* depth */
	R2V_PACKET_WRITE_UINT8(v->packet, 24);
	/* big-endian-flag */
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	/* true-colour-flag */
	R2V_PACKET_WRITE_UINT8(v->packet, 1);
	/* red-max */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, 255);
	/* green-max */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, 255);
	/* blue-max */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, 255);
	/* red-shift */
	R2V_PACKET_WRITE_UINT8(v->packet, 16);
	/* green-shift */
	R2V_PACKET_WRITE_UINT8(v->packet, 8);
	/* blue-shift */
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	/* padding */
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}

	/* send SetEncodings message */
	r2v_packet_reset(v->packet);
	R2V_PACKET_WRITE_UINT8(v->packet, RFB_SET_ENCODINGS);
	R2V_PACKET_WRITE_UINT8(v->packet, 0);
	R2V_PACKET_WRITE_UINT16_BE(v->packet, 2);
	R2V_PACKET_WRITE_UINT32_BE(v->packet, RFB_ENCODING_RAW);
	R2V_PACKET_WRITE_UINT32_BE(v->packet, RFB_ENCODING_COPYRECT);
	//R2V_PACKET_WRITE_UINT32_BE(v->packet, RFB_ENCODING_CURSOR);
	//R2V_PACKET_WRITE_UINT32_BE(v->packet, RFB_ENCODING_DESKTOP_SIZE);
	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}

	/* send FramebufferUpdateRequest message */
	if (r2v_vnc_send_fb_update_req(v, 0, 0, 0, v->framebuffer_width,
								   v->framebuffer_height) == -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

r2v_vnc_t *
r2v_vnc_init(int server_fd, const char *password, r2v_session_t *s)
{
	r2v_vnc_t *v = NULL;

	v = (r2v_vnc_t *)malloc(sizeof(r2v_vnc_t));
	if (v == NULL) {
		goto fail;
	}
	memset(v, 0, sizeof(r2v_vnc_t));

	v->session = s;

	v->fd = server_fd;
	v->packet = r2v_packet_init(65535);
	if (v->packet == NULL) {
		goto fail;
	}
	v->buffer = NULL;
	v->buffer_size = 0;

	if (password != NULL) {
		strncpy(v->password, password, sizeof(v->password));
	}

	if (r2v_vnc_build_conn(v) == -1) {
		goto fail;
	}

	return v;

fail:
	r2v_vnc_destory(v);
	return NULL;
}

void
r2v_vnc_destory(r2v_vnc_t *v)
{
	if (v == NULL) {
		return;
	}
	if (v->fd != 0) {
		close(v->fd);
	}
	if (v->packet != NULL) {
		r2v_packet_destory(v->packet);
	}
	free(v);
}

static int
r2v_vnc_process_raw_encoding(r2v_vnc_t *v, uint16_t x, uint16_t y,
							 uint16_t w, uint16_t h)
{
	const uint32_t max_byte_per_packet = 8192;

	uint16_t left, top, right, bottom, width, height, i;
	uint32_t data_size = w * h * 4;
	uint32_t line_size = w * 4;
	uint32_t max_line_per_packet = max_byte_per_packet / line_size;
	uint32_t line_per_packet;

	/* if data size is larger than vnc packet's buffer, 
	 * init a new packet with a larger buffer */
	if (data_size > v->packet->max_len) {
		r2v_packet_destory(v->packet);
		v->packet = r2v_packet_init(data_size);
		if (v->packet == NULL) {
			goto fail;
		}
	}
	if (r2v_vnc_recv1(v, data_size) == -1) {
		goto fail;
	}

	if (line_size > v->buffer_size) {
		v->buffer_size = line_size;
		v->buffer = (uint8_t *)realloc(v->buffer, v->buffer_size);
		if (v->buffer == NULL) {
			r2v_log_error("failed to allocate memory for swap buffer");
			goto fail;
		}
	}
	for (i = 0; i < h / 2; i++) {
		memcpy(v->buffer, v->packet->data + i * line_size, line_size);
		memcpy(v->packet->data + i * line_size,
			   v->packet->data + (h - i - 1) * line_size,
			   line_size);
		memcpy(v->packet->data + (h - i - 1) * line_size, v->buffer, line_size);
	}

/*
	left = x;
	top = y;
	right = x + w - 1;
	bottom = y + h - 1;
	width = w;
	height = h;
	if (r2v_rdp_send_bitmap_update(v->session->rdp, left, top, right, bottom,
								   width, height, 32, data_size,
								   v->packet->data) == -1) {
		goto fail;
	}
*/

	for (i = 0; i < h;) {
		if (i + max_line_per_packet > h) {
			line_per_packet = h - i;
		} else {
			line_per_packet = max_line_per_packet;
		}
		left = x;
		top = y + h - i - line_per_packet;
		right = x + w - 1;
		bottom = y + h - i - 1;
		width = w;
		height = line_per_packet;
		if (r2v_rdp_send_bitmap_update(v->session->rdp,
									   left, top, right, bottom,
									   width, height, 32,
									   line_size * line_per_packet,
									   v->packet->data + i * line_size) == -1) {
			goto fail;
		}
		i += line_per_packet;
	}

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_process_copy_rect_encoding(r2v_vnc_t *v, uint16_t x, uint16_t y,
								   uint16_t w, uint16_t h)
{
	uint16_t src_x, src_y;

	if (r2v_vnc_recv1(v, 4) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_UINT16_BE(v->packet, src_x);
	R2V_PACKET_READ_UINT16_BE(v->packet, src_y);
	r2v_log_debug("copy rect from src_x: %d src_y: %d", src_x, src_y);

	if (r2v_rdp_send_scrblt_order(v->session->rdp, x, y, w, h, src_x, src_y)
		== -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_process_framebuffer_update(r2v_vnc_t *v)
{
	uint16_t nrects = 0, i = 0, x, y, w, h;
	int32_t encoding_type;

	if (r2v_vnc_recv1(v, 3) == -1) {
		goto fail;
	}
	R2V_PACKET_SEEK_UINT8(v->packet);
	R2V_PACKET_READ_UINT16_BE(v->packet, nrects);
	//r2v_log_debug("receive framebuffer update with %d rects", nrects);

	for (i = 0; i < nrects; i++) {
		if (r2v_vnc_recv1(v, 12) == -1) {
			goto fail;
		}
		R2V_PACKET_READ_UINT16_BE(v->packet, x);
		R2V_PACKET_READ_UINT16_BE(v->packet, y);
		R2V_PACKET_READ_UINT16_BE(v->packet, w);
		R2V_PACKET_READ_UINT16_BE(v->packet, h);
		R2V_PACKET_READ_UINT32_BE(v->packet, encoding_type);
		//r2v_log_debug("rect %d of %d: pos: %d,%d size: %dx%d encoding: %d",
		//			  i + 1, nrects, x, y, w, h, encoding_type);

		switch (encoding_type) {
		case RFB_ENCODING_RAW:
			if (r2v_vnc_process_raw_encoding(v, x, y, w, h) == -1) {
				goto fail;
			}
			break;
		case RFB_ENCODING_COPYRECT:
			if (r2v_vnc_process_copy_rect_encoding(v, x, y, w, h) == -1) {
				goto fail;
			}
			break;
		default:
			r2v_log_warn("unknown encoding type: %d", encoding_type);
			break;
		}
	}

	/* send FramebufferUpdateRequest message */
	if (r2v_vnc_send_fb_update_req(v, 1, 0, 0, v->framebuffer_width,
								   v->framebuffer_height) == -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

static int
r2v_vnc_process_server_cut_text(r2v_vnc_t *v)
{
	uint32_t length;

	if (r2v_vnc_recv1(v, 7) == -1) {
		goto fail;
	}
	R2V_PACKET_SEEK(v->packet, 3);
	R2V_PACKET_READ_UINT32_BE(v->packet, length);
	if (r2v_vnc_recv1(v, length) == -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

int
r2v_vnc_process(r2v_vnc_t *v)
{
	uint8_t msg_type;

	if (r2v_vnc_recv1(v, 1) == -1) {
		goto fail;
	}
	R2V_PACKET_READ_UINT8(v->packet, msg_type);

	switch (msg_type) {
	case RFB_FRAMEBUFFER_UPDATE:
		if (r2v_vnc_process_framebuffer_update(v) == -1) {
			goto fail;
		}
		break;
	case RFB_SERVER_CUT_TEXT:
		if (r2v_vnc_process_server_cut_text(v) == -1) {
			goto fail;
		}
		break;
	default:
		r2v_log_debug("reveive unknown message type %d from vnc server",
					  msg_type);
		goto fail;
		break;
	}

	return 0;

fail:
	return -1;
}

int
r2v_vnc_send_fb_update_req(r2v_vnc_t *v, uint8_t incremental,
						   uint16_t x_pos, uint16_t y_pos,
						   uint16_t width, uint16_t height)
{
	/* if client sent Suppress Output PDU, then don't send framebuffer
	 * update request */
	if (v->session->rdp != NULL &&
		v->session->rdp->allow_display_updates == SUPPRESS_DISPLAY_UPDATES) {
		return 0;
	}

	/* send FramebufferUpdateRequest message */
	r2v_packet_reset(v->packet);

	/* message-type */
	R2V_PACKET_WRITE_UINT8(v->packet, RFB_FRAMEBUFFER_UPDATE_REQUEST);
	/* incremental */
	R2V_PACKET_WRITE_UINT8(v->packet, incremental);
	/* x-position */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, x_pos);
	/* y-position */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, y_pos);
	/* width */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, width);
	/* height */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, height);

	R2V_PACKET_END(v->packet);
	if (r2v_vnc_send(v) == -1) {
		goto fail;
	}

	return 0;

fail:
	return -1;
}

int
r2v_vnc_send_key_event(r2v_vnc_t *v, uint8_t down_flag, uint32_t key)
{
	/* send KeyEvent message */
	r2v_packet_reset(v->packet);
	/* message-type */
	R2V_PACKET_WRITE_UINT8(v->packet, RFB_KEY_EVENT);
	/* down-flag */
	R2V_PACKET_WRITE_UINT8(v->packet, down_flag);
	/* padding */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, 0);
	/* key */
	R2V_PACKET_WRITE_UINT32_BE(v->packet, key);
	R2V_PACKET_END(v->packet);

	return r2v_vnc_send(v);
}

int
r2v_vnc_send_pointer_event(r2v_vnc_t *v, uint8_t btn_mask,
						   uint16_t x_pos, uint16_t y_pos)
{
	/* send PointerEvent message */
	r2v_packet_reset(v->packet);
	/* message-type */
	R2V_PACKET_WRITE_UINT8(v->packet, RFB_POINTER_EVENT);
	/* button-mask */
	R2V_PACKET_WRITE_UINT8(v->packet, btn_mask);
	/* x-position */
	R2V_PACKET_WRITE_UINT16_BE(v->packet, x_pos);
	R2V_PACKET_WRITE_UINT16_BE(v->packet, y_pos);
	R2V_PACKET_END(v->packet);

	return r2v_vnc_send(v);
}
