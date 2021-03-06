/* Example program using libflowmanager to count flows in a trace file. 
 * Demonstrates how the libflowmanager API should be used to perform flow-based
 * measurements.
 *
 * Author: Shane Alcock
 */

#define __STDC_FORMAT_MACROS

#include <stdio.h>
#include <assert.h>
#include <getopt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include <libtrace.h>
#include <libflowmanager.h>

uint64_t flow_counter = 0;
uint64_t expired_flows = 0;

FlowManager *fm = NULL;

/* This data structure is used to demonstrate how to use the 'extension' 
 * pointer to store custom data for a flow */
typedef struct counter {
	uint64_t packets;
	uint8_t init_dir;
} CounterFlow;

/* Initialises the custom data for the given flow. Allocates memory for a
 * CounterFlow structure and ensures that the extension pointer points at
 * it.
 */
void init_counter_flow(Flow *f, uint8_t dir) {
	CounterFlow *cflow = NULL;

	cflow = (CounterFlow *)malloc(sizeof(CounterFlow));
	cflow->init_dir = dir;
	cflow->packets = 0;
	
	/* Don't forget to update our flow counter too! */
	flow_counter ++;
	f->extension = cflow;
}

/* Expires all flows that libflowmanager believes have been idle for too
 * long. The exp_flag variable tells libflowmanager whether it should force
 * expiry of all flows (e.g. if you have reached the end of the program and
 * want the stats for all the still-active flows). Otherwise, only flows
 * that have been idle for longer than their expiry timeout will be expired.
 */
void expire_counter_flows(double ts, bool exp_flag) {
        Flow *expired;

        /* Loop until libflowmanager has no more expired flows available */
	while ((expired = fm->expireNextFlow(ts, exp_flag)) != NULL) {

                CounterFlow *cflow = (CounterFlow *)expired->extension;
		
		/* We could do something with the packet count here, e.g.
		 * print it to stdout, but that would just produce lots of
		 * noisy output for no real benefit */


		/* Don't forget to free our custom data structure */
                free(cflow);

		/* VERY IMPORTANT: release the Flow structure itself so
                 * that libflowmanager can now safely delete the flow */
                fm->releaseFlow(expired);

		expired_flows ++;
        }
}


void per_packet(libtrace_packet_t *packet) {

        Flow *f;
        CounterFlow *cflow = NULL;
        uint8_t dir;
        bool is_new = false;

        libtrace_tcp_t *tcp = NULL;
        libtrace_ip_t *ip = NULL;
        double ts;

        uint16_t l3_type;

        /* Libflowmanager only deals with IP traffic, so ignore anything
	 * that does not have an IP header */
        ip = (libtrace_ip_t *)trace_get_layer3(packet, &l3_type, NULL);
        if (l3_type != 0x0800) return;
        if (ip == NULL) return;


	/* Many trace formats do not support direction tagging (e.g. PCAP), so
	 * using trace_get_direction() is not an ideal approach. The one we
	 * use here is not the nicest, but it is pretty consistent and 
	 * reliable. Feel free to replace this with something more suitable
	 * for your own needs!.
	 */
        if (ip->ip_src.s_addr < ip->ip_dst.s_addr)
                dir = 0;
        else
                dir = 1;

        /* Ignore packets where the IP addresses are the same - something is
         * probably screwy and it's REALLY hard to determine direction */
        if (ip->ip_src.s_addr == ip->ip_dst.s_addr)
                return;


        /* Match the packet to a Flow - this will create a new flow if
	 * there is no matching flow already in the Flow map and set the
	 * is_new flag to true. */
        f = fm->matchPacketToFlow(packet, dir, &is_new);

	/* Libflowmanager did not like something about that packet - best to
	 * just ignore it and carry on */
        if (f == NULL)
                return;

	/* If the returned flow is new, you will probably want to allocate and
	 * initialise any custom data that you intend to track for the flow */
        if (is_new)
                init_counter_flow(f, dir);
	
	/* Cast the extension pointer to match the custom data type */	
        cflow = (CounterFlow *)f->extension;

	/* Increment our packet counter */
	cflow->packets ++;

        /* Tell libflowmanager to update the expiry time for this flow */
        ts = trace_get_seconds(packet);
        fm->updateFlowExpiry(f, packet, dir, ts);

	/* Expire all suitably idle flows */
        expire_counter_flows(ts, false);

}


int main(int argc, char *argv[]) {

        libtrace_t *trace;
        libtrace_packet_t *packet;

        bool opt_true = true;
        bool opt_false = false;
        double ts;
	lfm_plugin_id_t plugid = LFM_PLUGIN_STANDARD;

        int i;

        packet = trace_create_packet();
        if (packet == NULL) {
                perror("Creating libtrace packet");
                return -1;
        }

        fm = new FlowManager();

	/* This tells libflowmanager to ignore any flows where an RFC1918
	 * private IP address is involved */
        if (fm->setConfigOption(LFM_CONFIG_IGNORE_RFC1918, &opt_true) == 0)
                return -1;

	/* This tells libflowmanager not to replicate the TCP timewait
	 * behaviour where closed TCP connections are retained in the Flow
	 * map for an extra 2 minutes */
        if (fm->setConfigOption(LFM_CONFIG_TCP_TIMEWAIT, &opt_false) == 0)
                return -1;

	/* This tells libflowmanager to use the standard set of flow expiry
	 * rules, i.e. the original libflowmanager expiry rules.
	 *
	 * Other possible rulesets are:
	 *  LFM_PLUGIN_STANDARD_SHORT_UDP -- same as standard but with fast
	 *  expiry for short UDP flows. 
	 *  LFM_PLUGIN_FIXED_INACTIVE -- flows expire after a fixed period of
	 *  inactivity regardless of flow state or transport protocol.
	 *
	 * Expiry thresholds for the other rulesets can be set using the
	 * LFM_CONFIG_FIXED_EXPIRY_THRESHOLD config option. For the Short UDP
	 * ruleset, this will set the inactivity threshold for the short UDP
	 * flows (default is 10 seconds). For Fixed Inactive, this will set
	 * the inactivity threshold for all flows (default is 60 seconds).
	 */
	if (fm->setConfigOption(LFM_CONFIG_EXPIRY_PLUGIN, &plugid) == 0)
		return -1;

        optind = 1;

        for (i = optind; i < argc; i++) {

                printf("%s\n", argv[i]);
                
		/* Bog-standard libtrace stuff for reading trace files */
		trace = trace_create(argv[i]);

                if (!trace) {
                        perror("Creating libtrace trace");
                        return -1;
                }

                if (trace_is_err(trace)) {
                        trace_perror(trace, "Opening trace file");
                        trace_destroy(trace);
                        continue;
                }

                if (trace_start(trace) == -1) {
                        trace_perror(trace, "Starting trace");
                        trace_destroy(trace);
                        continue;
                }
                while (trace_read_packet(trace, packet) > 0) {
                        ts = trace_get_seconds(packet);
			per_packet(packet);

                }

                if (trace_is_err(trace)) {
                        trace_perror(trace, "Reading packets");
                        trace_destroy(trace);
                        continue;
                }

                trace_destroy(trace);

        }

        trace_destroy_packet(packet);

	/* And finally, print something useful to make the exercise 
	 * worthwhile */
	printf("Final count: %" PRIu64 "\n", flow_counter);
	printf("Expired flows: %" PRIu64 "\n", expired_flows);

	expire_counter_flows(ts, true);
        delete(fm);
        return 0;

}

