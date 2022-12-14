/* M3UA/SUA [S]SNM Handling */

/* (C) 2021 by Harald Welte <laforge@gnumonks.org>
 * All Rights Reserved
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <stdint.h>

#include <osmocom/core/utils.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/linuxlist.h>

#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/protocol/m3ua.h>
#include <osmocom/sigtran/protocol/sua.h>
#include <osmocom/sigtran/protocol/mtp.h>

#include "xua_internal.h"
#include "sccp_internal.h"

/* we can share this code between M3UA and SUA as the below conditions are true */
osmo_static_assert(M3UA_SNM_DUNA == SUA_SNM_DUNA, _sa_duna);
osmo_static_assert(M3UA_SNM_DAVA == SUA_SNM_DAVA, _sa_dava);
osmo_static_assert(M3UA_SNM_DAUD == SUA_SNM_DAUD, _sa_dava);
osmo_static_assert(M3UA_IEI_AFFECTED_PC == SUA_IEI_AFFECTED_PC, _sa_aff_pc);
osmo_static_assert(M3UA_IEI_ROUTE_CTX == SUA_IEI_ROUTE_CTX, _sa_rctx);
osmo_static_assert(M3UA_IEI_INFO_STRING == SUA_IEI_INFO_STRING, _sa_inf_str);

static const char *format_affected_pcs_c(void *ctx, const struct osmo_ss7_instance *s7i,
					 const struct xua_msg_part *ie_aff_pc)
{
	const uint32_t *aff_pc = (const uint32_t *) ie_aff_pc->dat;
	unsigned int num_aff_pc = ie_aff_pc->len / sizeof(uint32_t);
	char *out = talloc_strdup(ctx, "");
	int i;

	for (i = 0; i < num_aff_pc; i++) {
		uint32_t _aff_pc = ntohl(aff_pc[i]);
		uint32_t pc = _aff_pc & 0xffffff;
		uint8_t mask = _aff_pc >> 24;

		/* append point code + mask */
		out = talloc_asprintf_append(out, "%s%s/%u, ", i == 0 ? "" : ", ",
					     osmo_ss7_pointcode_print(s7i, pc), mask);
	}
	return out;
}

/* obtain all routing contexts (in network byte order) that exist within the given ASP */
static unsigned int get_all_rctx_for_asp(uint32_t *rctx, unsigned int rctx_size,
					 struct osmo_ss7_asp *asp, struct osmo_ss7_as *excl_as)
{
	unsigned int count = 0;
	struct osmo_ss7_as *as;

	llist_for_each_entry(as, &asp->inst->as_list, list) {
		if (as == excl_as)
			continue;
		if (!osmo_ss7_as_has_asp(as, asp))
			continue;
		if (as->cfg.routing_key.context == 0)
			continue;
		if (count >= rctx_size)
			break;
		rctx[count] = htonl(as->cfg.routing_key.context);
		count++;
	}
	return count;
}

static void xua_tx_snm_available(struct osmo_ss7_asp *asp, const uint32_t *rctx, unsigned int num_rctx,
				 const uint32_t *aff_pc, unsigned int num_aff_pc,
				 const char *info_str, bool available)
{
	switch (asp->cfg.proto) {
	case OSMO_SS7_ASP_PROT_M3UA:
		m3ua_tx_snm_available(asp, rctx, num_rctx, aff_pc, num_aff_pc, info_str, available);
		break;
	case OSMO_SS7_ASP_PROT_SUA:
		sua_tx_snm_available(asp, rctx, num_rctx, aff_pc, num_aff_pc, NULL, NULL, info_str, available);
		break;
	default:
		break;
	}
}

static void xua_tx_upu(struct osmo_ss7_asp *asp, const uint32_t *rctx, unsigned int num_rctx,
			uint32_t dpc, uint16_t user, uint16_t cause, const char *info_str)
{
	switch (asp->cfg.proto) {
	case OSMO_SS7_ASP_PROT_M3UA:
		m3ua_tx_dupu(asp, rctx, num_rctx, dpc, user, cause, info_str);
		break;
	case OSMO_SS7_ASP_PROT_SUA:
		sua_tx_dupu(asp, rctx, num_rctx, dpc, user, cause, info_str);
		break;
	default:
		break;
	}
}

static void xua_tx_scon(struct osmo_ss7_asp *asp, const uint32_t *rctx, unsigned int num_rctx,
			const uint32_t *aff_pc, unsigned int num_aff_pc,
			const uint32_t *concerned_dpc, const uint8_t *cong_level,
			const char *info_string)
{
	switch (asp->cfg.proto) {
	case OSMO_SS7_ASP_PROT_M3UA:
		m3ua_tx_snm_congestion(asp, rctx, num_rctx, aff_pc, num_aff_pc,
				       concerned_dpc, cong_level, info_string);
		break;
	case OSMO_SS7_ASP_PROT_SUA:
		sua_tx_snm_congestion(asp, rctx, num_rctx, aff_pc, num_aff_pc, NULL,
				      cong_level ? *cong_level : 0, info_string);
		break;
	default:
		break;
	}
}

/* generate MTP-PAUSE / MTP-RESUME towards local SCCP users */
static void xua_snm_pc_available_to_sccp(struct osmo_sccp_instance *sccp,
					 const uint32_t *aff_pc, unsigned int num_aff_pc,
					 bool available)
{
	int i;
	for (i = 0; i < num_aff_pc; i++) {
		uint32_t _aff_pc = ntohl(aff_pc[i]);
		uint32_t pc = _aff_pc & 0xffffff;
		uint8_t mask = _aff_pc >> 24;

		if (!mask) {
			if (available)
				sccp_scmg_rx_mtp_resume(sccp, pc);
			else
				sccp_scmg_rx_mtp_pause(sccp, pc);
		} else {
			/* we have to send one MTP primitive for each individual point
			 * code within that mask */
			uint32_t maskbits = (1 << mask) - 1;
			uint32_t fullpc;
			for (fullpc = (pc & ~maskbits); fullpc <= (pc | maskbits); fullpc++) {
				if (available)
					sccp_scmg_rx_mtp_resume(sccp, pc);
				else
					sccp_scmg_rx_mtp_pause(sccp, pc);
			}
		}
	}
}

/* advertise availability of point codes (with masks) */
void xua_snm_pc_available(struct osmo_ss7_as *as, const uint32_t *aff_pc,
			  unsigned int num_aff_pc, const char *info_str, bool available)
{
	struct osmo_ss7_instance *s7i = as->inst;
	struct osmo_ss7_asp *asp;
	uint32_t rctx[32];
	unsigned int num_rctx;

	/* inform local users via a MTP-{PAUSE, RESUME} primitive */
	if (s7i->sccp)
		xua_snm_pc_available_to_sccp(s7i->sccp, aff_pc, num_aff_pc, available);

	/* inform remote ASPs via DUNA/DAVA */
	llist_for_each_entry(asp, &s7i->asp_list, list) {
		/* SSNM is only permitted for ASPs in ACTIVE state */
		if (!osmo_ss7_asp_active(asp))
			continue;

		/* only send DAVA/DUNA if we locally are the SG and the remote is ASP */
		if (asp->cfg.role != OSMO_SS7_ASP_ROLE_SG)
			continue;

		num_rctx = get_all_rctx_for_asp(rctx, ARRAY_SIZE(rctx), asp, as);
		/* this can happen if the given ASP is only in the AS that reports the change,
		 * which shall be excluded */
		if (num_rctx == 0)
			continue;
		xua_tx_snm_available(asp, rctx, num_rctx, aff_pc, num_aff_pc, info_str, available);
	}
}

/* generate SS-PROHIBITED / SS-ALLOWED towards local SCCP users */
static void sua_snm_ssn_available_to_sccp(struct osmo_sccp_instance *sccp, uint32_t aff_pc,
					  uint32_t aff_ssn, uint32_t smi, bool available)
{
	if (available)
		sccp_scmg_rx_ssn_allowed(sccp, aff_pc, aff_ssn, smi);
	else
		sccp_scmg_rx_ssn_prohibited(sccp, aff_pc, aff_ssn, smi);
}

/* advertise availability of a single subsystem */
static void sua_snm_ssn_available(struct osmo_ss7_as *as, uint32_t aff_pc, uint32_t aff_ssn,
				  const uint32_t *smi, const char *info_str, bool available)
{
	struct osmo_ss7_instance *s7i = as->inst;
	struct osmo_ss7_asp *asp;
	uint32_t rctx[32];
	unsigned int num_rctx;
	uint32_t _smi = smi ? *smi : 0; /* 0 == reserved/unknown in SUA */

	if (s7i->sccp)
		sua_snm_ssn_available_to_sccp(s7i->sccp, aff_pc, aff_ssn, _smi, available);

	/* inform remote SUA ASPs via DUNA/DAVA */
	llist_for_each_entry(asp, &s7i->asp_list, list) {
		/* SSNM is only permitted for ASPs in ACTIVE state */
		if (!osmo_ss7_asp_active(asp))
			continue;

		/* only send DAVA/DUNA if we locally are the SG and the remote is ASP */
		if (asp->cfg.role != OSMO_SS7_ASP_ROLE_SG)
			continue;

		/* DUNA/DAVA for SSN only exists in SUA */
		if (asp->cfg.proto != OSMO_SS7_ASP_PROT_SUA)
			continue;

		num_rctx = get_all_rctx_for_asp(rctx, ARRAY_SIZE(rctx), asp, as);
		/* this can happen if the given ASP is only in the AS that reports the change,
		 * which shall be excluded */
		if (num_rctx == 0)
			continue;
		sua_tx_snm_available(asp, rctx, num_rctx, &aff_pc, 1, &aff_ssn, smi, info_str, available);
	}
}

static void xua_snm_upu(struct osmo_ss7_as *as, uint32_t dpc, uint16_t user, uint16_t cause,
			const char *info_str)
{
	struct osmo_ss7_instance *s7i = as->inst;
	struct osmo_ss7_asp *asp;
	uint32_t rctx[32];
	unsigned int num_rctx;

	/* Translate to MTP-STATUS.ind towards SCCP (will create N-PCSTATE.ind to SCU) */
	if (s7i->sccp && user == MTP_SI_SCCP)
		sccp_scmg_rx_mtp_status(s7i->sccp, dpc, cause);

	/* inform remote ASPs via DUPU */
	llist_for_each_entry(asp, &s7i->asp_list, list) {
		/* SSNM is only permitted for ASPs in ACTIVE state */
		if (!osmo_ss7_asp_active(asp))
			continue;

		/* only send DAVA/DUNA if we locally are the SG and the remote is ASP */
		if (asp->cfg.role != OSMO_SS7_ASP_ROLE_SG)
			continue;

		num_rctx = get_all_rctx_for_asp(rctx, ARRAY_SIZE(rctx), asp, as);
		/* this can happen if the given ASP is only in the AS that reports the change,
		 * which shall be excluded */
		if (num_rctx == 0)
			continue;

		xua_tx_upu(asp, rctx, num_rctx, dpc, user, cause, info_str);
	}
}

static void xua_snm_scon(struct osmo_ss7_as *as, const uint32_t *aff_pc, unsigned int num_aff_pc,
			 const uint32_t *concerned_dpc, const uint8_t *cong_level, const char *info_string)
{
	struct osmo_ss7_instance *s7i = as->inst;
	struct osmo_ss7_asp *asp;
	uint32_t rctx[32];
	unsigned int num_rctx;

	/* TODO: How to translate to MTP and towards SCCP (create N-PCSTATE.ind to SCU) */

	/* inform remote ASPs via SCON */
	llist_for_each_entry(asp, &s7i->asp_list, list) {
		/* SSNM is only permitted for ASPs in ACTIVE state */
		if (!osmo_ss7_asp_active(asp))
			continue;

		/* only send SCON if we locally are the SG and the remote is ASP */
		if (asp->cfg.role != OSMO_SS7_ASP_ROLE_SG)
			continue;

		num_rctx = get_all_rctx_for_asp(rctx, ARRAY_SIZE(rctx), asp, as);
		/* this can happen if the given ASP is only in the AS that reports the change,
		 * which shall be excluded */
		if (num_rctx == 0)
			continue;

		xua_tx_scon(asp, rctx, num_rctx, aff_pc, num_aff_pc, concerned_dpc, cong_level, info_string);
	}
}

/* receive DAUD from ASP; pc is 'affected PC' IE with mask in network byte order! */
void xua_snm_rx_daud(struct osmo_ss7_asp *asp, struct xua_msg *xua)
{
	struct xua_msg_part *ie_aff_pc = xua_msg_find_tag(xua, M3UA_IEI_AFFECTED_PC);
	const char *info_str = xua_msg_get_str(xua, M3UA_IEI_INFO_STRING);
	struct osmo_ss7_instance *s7i = asp->inst;
	unsigned int num_aff_pc;
	unsigned int num_rctx;
	const uint32_t *aff_pc;
	uint32_t rctx[32];
	int log_ss = osmo_ss7_asp_get_log_subsys(asp);
	int i;

	OSMO_ASSERT(ie_aff_pc);
	aff_pc = (const uint32_t *) ie_aff_pc->dat;
	num_aff_pc = ie_aff_pc->len / sizeof(uint32_t);

	num_rctx = get_all_rctx_for_asp(rctx, ARRAY_SIZE(rctx), asp, NULL);

	LOGPASP(asp, log_ss, LOGL_INFO, "Rx DAUD(%s) for %s\n", info_str ? info_str : "",
		format_affected_pcs_c(xua, asp->inst, ie_aff_pc));

	/* iterate over list of point codes, generate DAVA/DUPU */
	for (i = 0; i < num_aff_pc; i++) {
		uint32_t _aff_pc = ntohl(aff_pc[i]);
		uint32_t pc = _aff_pc & 0xffffff;
		uint8_t mask = _aff_pc >> 24;
		bool is_available = false;

		if (mask == 0) {
			/* one single point code */

			/* FIXME: don't just check for a route; but also check if the route is "active" */
			if (osmo_ss7_route_lookup(s7i, pc))
				is_available = true;

			xua_tx_snm_available(asp, rctx, num_rctx, &aff_pc[i], 1, "Response to DAUD",
					     is_available);
		} else {
			/* TODO: wildcard match */
			LOGPASP(asp, log_ss, LOGL_NOTICE, "DAUD with wildcard match not supported yet\n");
		}
	}
}

/* an incoming xUA DUNA was received from a remote SG */
void xua_snm_rx_duna(struct osmo_ss7_asp *asp, struct osmo_ss7_as *as, struct xua_msg *xua)
{
	struct xua_msg_part *ie_aff_pc = xua_msg_find_tag(xua, M3UA_IEI_AFFECTED_PC);
	struct xua_msg_part *ie_ssn = xua_msg_find_tag(xua, SUA_IEI_SSN);
	const char *info_str = xua_msg_get_str(xua, M3UA_IEI_INFO_STRING);
	/* TODO: should our processing depend on the RCTX included? I somehow don't think so */
	//struct xua_msg_part *ie_rctx = xua_msg_find_tag(xua, M3UA_IEI_ROUTE_CTX);
	int log_ss = osmo_ss7_asp_get_log_subsys(asp);

	OSMO_ASSERT(ie_aff_pc);

	if (asp->cfg.role != OSMO_SS7_ASP_ROLE_ASP)
		return;

	LOGPASP(asp, log_ss, LOGL_NOTICE, "Rx DUNA(%s) for %s\n", info_str ? info_str : "",
		format_affected_pcs_c(xua, asp->inst, ie_aff_pc));

	if (asp->cfg.proto == OSMO_SS7_ASP_PROT_SUA && ie_ssn) {
		/* when the SSN is included, DUNA corresponds to the SCCP N-STATE primitive */
		uint32_t ssn = xua_msg_part_get_u32(ie_ssn);
		const uint32_t *aff_pc = (const uint32_t *)ie_aff_pc->dat;
		uint32_t pc, smi;
		/* The Affected Point Code can only contain one point code when SSN is present */
		if (ie_aff_pc->len/sizeof(uint32_t) != 1)
			return;
		pc = ntohl(aff_pc[0]) & 0xffffff;
		sua_snm_ssn_available(as, pc, ssn, xua_msg_get_u32p(xua, SUA_IEI_SMI, &smi), info_str, false);
	} else {
		/* when the SSN is not included, DUNA corresponds to the SCCP N-PCSTATE primitive */
		xua_snm_pc_available(as, (const uint32_t *)ie_aff_pc->dat,
				     ie_aff_pc->len / sizeof(uint32_t), info_str, false);
	}
}

/* an incoming xUA DAVA was received from a remote SG */
void xua_snm_rx_dava(struct osmo_ss7_asp *asp, struct osmo_ss7_as *as, struct xua_msg *xua)
{
	struct xua_msg_part *ie_aff_pc = xua_msg_find_tag(xua, M3UA_IEI_AFFECTED_PC);
	struct xua_msg_part *ie_ssn = xua_msg_find_tag(xua, SUA_IEI_SSN);
	const char *info_str = xua_msg_get_str(xua, M3UA_IEI_INFO_STRING);
	/* TODO: should our processing depend on the RCTX included? I somehow don't think so */
	//struct xua_msg_part *ie_rctx = xua_msg_find_tag(xua, M3UA_IEI_ROUTE_CTX);
	int log_ss = osmo_ss7_asp_get_log_subsys(asp);

	OSMO_ASSERT(ie_aff_pc);

	if (asp->cfg.role != OSMO_SS7_ASP_ROLE_ASP)
		return;

	LOGPASP(asp, log_ss, LOGL_NOTICE, "Rx DAVA(%s) for %s\n", info_str ? info_str : "",
		format_affected_pcs_c(xua, asp->inst, ie_aff_pc));

	if (asp->cfg.proto == OSMO_SS7_ASP_PROT_SUA && ie_ssn) {
		/* when the SSN is included, DAVA corresponds to the SCCP N-STATE primitive */
		uint32_t ssn = xua_msg_part_get_u32(ie_ssn);
		const uint32_t *aff_pc = (const uint32_t *)ie_aff_pc->dat;
		uint32_t pc, smi;
		/* The Affected Point Code can only contain one point code when SSN is present */
		if (ie_aff_pc->len/sizeof(uint32_t) != 1)
			return;
		pc = ntohl(aff_pc[0]) & 0xffffff;
		sua_snm_ssn_available(as, pc, ssn, xua_msg_get_u32p(xua, SUA_IEI_SMI, &smi), info_str, true);
	} else {
		/* when the SSN is not included, DAVA corresponds to the SCCP N-PCSTATE primitive */
		xua_snm_pc_available(as, (const uint32_t *)ie_aff_pc->dat,
				     ie_aff_pc->len / sizeof(uint32_t), info_str, true);
	}
}

/* an incoming SUA/M3UA DUPU was received from a remote SG */
void xua_snm_rx_dupu(struct osmo_ss7_asp *asp, struct osmo_ss7_as *as, struct xua_msg *xua)
{
	uint32_t aff_pc = xua_msg_get_u32(xua, SUA_IEI_AFFECTED_PC);
	const char *info_str = xua_msg_get_str(xua, SUA_IEI_INFO_STRING);
	/* TODO: should our processing depend on the RCTX included? I somehow don't think so */
	//struct xua_msg_part *ie_rctx = xua_msg_find_tag(xua, SUA_IEI_ROUTE_CTX);
	int log_ss = osmo_ss7_asp_get_log_subsys(asp);
	uint32_t cause_user;
	uint16_t cause, user;

	if (asp->cfg.role != OSMO_SS7_ASP_ROLE_ASP)
		return;

	switch (asp->cfg.proto) {
	case OSMO_SS7_ASP_PROT_M3UA:
		cause_user = xua_msg_get_u32(xua, M3UA_IEI_USER_CAUSE);
		break;
	case OSMO_SS7_ASP_PROT_SUA:
		cause_user = xua_msg_get_u32(xua, SUA_IEI_USER_CAUSE);
		break;
	default:
		return;
	}

	cause = cause_user >> 16;
	user = cause_user & 0xffff;
	LOGPASP(asp, log_ss, LOGL_NOTICE, "Rx DUPU(%s) for %s User %s, cause %u\n",
		info_str ? info_str : "", osmo_ss7_pointcode_print(asp->inst, aff_pc),
		get_value_string(mtp_si_vals, user), cause);

	xua_snm_upu(as, aff_pc, user, cause, info_str);
}

/* an incoming SUA/M3UA SCON was received from a remote SG */
void xua_snm_rx_scon(struct osmo_ss7_asp *asp, struct osmo_ss7_as *as, struct xua_msg *xua)
{
	struct xua_msg_part *ie_aff_pc = xua_msg_find_tag(xua, M3UA_IEI_AFFECTED_PC);
	const char *info_str = xua_msg_get_str(xua, M3UA_IEI_INFO_STRING);
	uint32_t _concerned_dpc, _cong_level;
	const uint32_t *concerned_dpc = xua_msg_get_u32p(xua, M3UA_IEI_CONC_DEST, &_concerned_dpc);
	const uint32_t *cong_level = xua_msg_get_u32p(xua, M3UA_IEI_CONG_IND, &_cong_level);
	int log_ss = osmo_ss7_asp_get_log_subsys(asp);

	OSMO_ASSERT(ie_aff_pc);

	LOGPASP(asp, log_ss, LOGL_NOTICE, "RX SCON(%s) for %s level=%u\n", info_str ? info_str : "",
		format_affected_pcs_c(xua, asp->inst, ie_aff_pc), cong_level ? *cong_level : 0);

	xua_snm_scon(as, (const uint32_t *) ie_aff_pc->dat, ie_aff_pc->len / sizeof(uint32_t),
		     concerned_dpc, (const uint8_t *) cong_level, info_str);
}
