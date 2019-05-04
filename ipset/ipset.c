#include "config.h"
#include "ipset/ipset.h"
#include "util/regional.h"
#include "util/config_file.h"

#include "services/cache/dns.h"

#include "sldns/sbuffer.h"
#include "sldns/wire2str.h"
#include "sldns/parseutil.h"

#include <libmnl/libmnl.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/ipset/ip_set.h>

#define BUFF_LEN 256

/**
 * Return an error
 * @param qstate: our query state
 * @param id: module id
 * @param rcode: error code (DNS errcode).
 * @return: 0 for use by caller, to make notation easy, like:
 * 	return error_response(..).
 */
static int error_response(struct module_qstate* qstate, int id, int rcode) {
	verbose(VERB_QUERY, "return error response %s",
		sldns_lookup_by_id(sldns_rcodes, rcode)?
		sldns_lookup_by_id(sldns_rcodes, rcode)->name:"??");
	qstate->return_rcode = rcode;
	qstate->return_msg = NULL;
	qstate->ext_state[id] = module_finished;
	return 0;
}

static int add_to_ipset(struct mnl_socket *mnl, const char *setname, const void *ipaddr, int af) {
	struct nlmsghdr *nlh;
	struct nfgenmsg *nfg;
	struct nlattr *nested[2];
	static char buffer[BUFF_LEN];

	if (strlen(setname) >= IPSET_MAXNAMELEN) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if (af != AF_INET && af != AF_INET6) {
		errno = EAFNOSUPPORT;
		return -1;
	}

	nlh = mnl_nlmsg_put_header(buffer);
	nlh->nlmsg_type = IPSET_CMD_ADD | (NFNL_SUBSYS_IPSET << 8);
	nlh->nlmsg_flags = NLM_F_REQUEST|NLM_F_ACK|NLM_F_EXCL;

	nfg = mnl_nlmsg_put_extra_header(nlh, sizeof(struct nfgenmsg));
	nfg->nfgen_family = af;
	nfg->version = NFNETLINK_V0;
	nfg->res_id = htons(0);

	mnl_attr_put_u8(nlh, IPSET_ATTR_PROTOCOL, IPSET_PROTOCOL);
	mnl_attr_put(nlh, IPSET_ATTR_SETNAME, strlen(setname) + 1, setname);
	nested[0] = mnl_attr_nest_start(nlh, IPSET_ATTR_DATA);
	nested[1] = mnl_attr_nest_start(nlh, IPSET_ATTR_IP);
	mnl_attr_put(nlh, (af == AF_INET ? IPSET_ATTR_IPADDR_IPV4 : IPSET_ATTR_IPADDR_IPV6)
			| NLA_F_NET_BYTEORDER, (af == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr)), ipaddr);
	mnl_attr_nest_end(nlh, nested[1]);
	mnl_attr_nest_end(nlh, nested[0]);

	if (mnl_socket_sendto(mnl, nlh, nlh->nlmsg_len) < 0) {
		return -1;
	}
	return 0;
}

static int ipset_update(struct module_env *env, struct dns_msg *return_msg, struct ipset_env *ie) {
	int ret;

	struct mnl_socket *mnl;

	int i, j;

	const char *setname;

	struct ub_packed_rrset_key *rrset;
	struct packed_rrset_data *d;

	int af;

	static char dname[BUFF_LEN];
	const char *s;
	int dlen, plen;

	struct config_strlist *p;

	uint16_t rrtype;
	size_t rr_len, rd_len;

	uint8_t *rr_data;

	mnl = (struct mnl_socket *)ie->mnl;
	if (mnl == NULL) {
		return -1;
	}

	for (i = 0; i < return_msg->rep->rrset_count; ++i) {
		setname = NULL;

		rrset = return_msg->rep->rrsets[i];

		if (rrset->rk.type == htons(LDNS_RR_TYPE_A)) {
			af = AF_INET;
			if ((ie->v4_enabled == 1)) {
				setname = ie->name_v4;
			}
		} else {
			af = AF_INET6;
			if ((ie->v6_enabled == 1)) {
				setname = ie->name_v6;
			}
		}

		if (setname != NULL) {
			dlen = sldns_wire2str_dname_buf(rrset->rk.dname, rrset->rk.dname_len, dname, BUFF_LEN);
			if (dlen == 0) {
				log_err("bad domain name");
				return -1;
			}
			if (dname[dlen - 1] == '.') {
				dlen--;
			}

			for (p = env->cfg->local_zones_ipset; p; p = p->next) {
				plen = strlen(p->str);

				if (dlen >= plen) {
					s = dname + (dlen - plen);

					if (strncasecmp(p->str, s, plen) == 0) {
						d = (struct packed_rrset_data*)rrset->entry.data;
						for (j = 0; j < d->count + d->rrsig_count; j++) {
							rr_len = d->rr_len[j];
							rr_data = d->rr_data[j];

							rd_len = sldns_read_uint16(rr_data);
							if (rr_len - 2 >= rd_len) {
								ret = add_to_ipset(mnl, setname, rr_data + 2, af);
								if (ret < 0) {
									log_err("ipset: could not add %s into %s", dname, setname);
									return ret;
								}
							}
						}
						break;
					}
				}
			}
	        }
	}

	return 0;
}

int ipset_init(struct module_env* env, int id) {
	struct mnl_socket *mnl;

	struct ipset_env *ipset_env = (struct ipset_env *)calloc(1,
		sizeof(struct ipset_env));
	if(!ipset_env) {
		log_err("malloc failure");
		return 0;
	}
	env->modinfo[id] = (void*)ipset_env;

	ipset_env->name_v4 = env->cfg->ipset_name_v4;
	ipset_env->name_v6 = env->cfg->ipset_name_v6;

	ipset_env->v4_enabled = (ipset_env->name_v4 == NULL) || (strlen(ipset_env->name_v4) == 0) ? 0 : 1;
	ipset_env->v6_enabled = (ipset_env->name_v6 == NULL) || (strlen(ipset_env->name_v6) == 0) ? 0 : 1;

	if ((ipset_env->v4_enabled < 1) && (ipset_env->v6_enabled < 1)) {
		log_err("ipset: set name no configuration?");
		return 0;
	}

	mnl = mnl_socket_open(NETLINK_NETFILTER);
	if (mnl <= 0) {
		log_err("ipset: could not open netfilter.");
		return 0;
	}

	if (mnl_socket_bind(mnl, 0, MNL_SOCKET_AUTOPID) < 0) {
		mnl_socket_close(mnl);
		log_err("ipset: could not bind netfilter.");
		return 0;
	}

	ipset_env->mnl = mnl;

	return 1;
}

void ipset_deinit(struct module_env *env, int id) {
	struct mnl_socket *mnl;

	struct ipset_env* ipset_env;
	if(!env || !env->modinfo[id])
		return;
	ipset_env = (struct ipset_env*)env->modinfo[id];

	mnl = (struct mnl_socket *)ipset_env->mnl;
	if (mnl) {
		mnl_socket_close(mnl);
		ipset_env->mnl = NULL;
	}

	free(ipset_env);
	env->modinfo[id] = NULL;
}

static int ipset_new(struct module_qstate* qstate, int id) {
	struct ipset_qstate* iq = (struct ipset_qstate*)regional_alloc(
		qstate->region, sizeof(struct ipset_qstate));
	qstate->minfo[id] = iq;
	if(!iq)
		return 0;
	memset(iq, 0, sizeof(*iq));
	/* initialise it */
	/* TODO */

	return 1;
}

void ipset_operate(struct module_qstate *qstate, enum module_ev event, int id,
	struct outbound_entry *outbound) {
	struct ipset_env *ie = (struct ipset_env *)qstate->env->modinfo[id];
	struct ipset_qstate *iq = (struct ipset_qstate *)qstate->minfo[id];
	verbose(VERB_QUERY, "ipset[module %d] operate: extstate:%s event:%s",
		id, strextstate(qstate->ext_state[id]), strmodulevent(event));
	if(iq) log_query_info(VERB_QUERY, "ipset operate: query",
		&qstate->qinfo);

	/* perform ipset state machine */
	if((event == module_event_new || event == module_event_pass) &&
		iq == NULL) {
		if(!ipset_new(qstate, id)) {
			(void)error_response(qstate, id, LDNS_RCODE_SERVFAIL);
			return;
		}
		iq = (struct ipset_qstate*)qstate->minfo[id];
	}

	if(iq && (event == module_event_pass || event == module_event_new)) {
		qstate->ext_state[id] = module_wait_module;
		return;
	}

	if(iq && (event == module_event_moddone)) {
		ipset_update(qstate->env, qstate->return_msg, ie);
		qstate->ext_state[id] = module_finished;
		return;
	}
	if(iq && outbound) {
		/* ipset does not need to process responses at this time
		 * ignore it.
		ipset_process_response(qstate, iq, ie, id, outbound, event);
		*/
		return;
	}
	if(event == module_event_error) {
		verbose(VERB_ALGO, "got called with event error, giving up");
		(void)error_response(qstate, id, LDNS_RCODE_SERVFAIL);
		return;
	}
	if(!iq && (event == module_event_moddone)) {
		/* during priming, module done but we never started */
		qstate->ext_state[id] = module_finished;
		return;
	}

	log_err("bad event for ipset");
	(void)error_response(qstate, id, LDNS_RCODE_SERVFAIL);
}

void ipset_inform_super(struct module_qstate* ATTR_UNUSED(qstate),
	int ATTR_UNUSED(id), struct module_qstate* ATTR_UNUSED(super)) {
	/* ipset does not use subordinate requests at this time */
	verbose(VERB_ALGO, "ipset inform_super was called");
}

void ipset_clear(struct module_qstate* qstate, int id) {
	struct cachedb_qstate* iq;
	if(!qstate)
		return;
	iq = (struct cachedb_qstate*)qstate->minfo[id];
	if(iq) {
		/* free contents of iq */
		/* TODO */
	}
	qstate->minfo[id] = NULL;
}

size_t ipset_get_mem(struct module_env* env, int id) {
	struct ipset_env *ie = (struct ipset_env *)env->modinfo[id];
	if (!ie) {
		return 0;
	}
	return sizeof(*ie);
}

/**
 * The ipset function block 
 */
static struct module_func_block ipset_block = {
	"ipset",
	&ipset_init, &ipset_deinit, &ipset_operate,
	&ipset_inform_super, &ipset_clear, &ipset_get_mem
};

struct module_func_block * ipset_get_funcblock(void) {
	return &ipset_block;
}


