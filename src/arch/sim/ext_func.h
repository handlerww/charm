/*****************************************************************************
 * $Source$
 * $Author$
 * $Date$
 * $Revision$
 *****************************************************************************/

static               insert();
static void          *remove_front();
static               read_parameters();
static int           comm_init();
static REL_TIME      latency();
static REL_TIME      broadcast_effect();
static int           network_ok_l();
static int           network_ok_s();
static int           network_ok_l_g();
static int           network_ok_s_g();
static int           cpu_accepts_l();
static int           cpu_accepts_s();
static int           scp_accepts_l();
static int           scp_accepts_s();
static int           always_accept();
static int           increase_net_load();
static int           decrease_net_load();
static int           ge();
static void          *create_event_heap();
static MSG           *fifo_dequeue();
static float         ran1();

static int sim_read_parameters();
static void simulate();
static cpu_event();
static recv_cp_event();
static recv_cp_deposit();
static send_cp_event();
static actual_broadcast();
static actual_send();
static wait_on_network();
static wait_on_local();
static release_waiting_network();
static release_waiting_local();
static update_etime();
static assign_etime();
static update_lower_bound();
static advance_clock();
static select_processor();
static sim_send_message();
static become_idle();
static make_active();
static int sim_initialize();
static init_max_time_const();
static int error_msg();
static relocate_event();
static next_event();
static SIM_TIME next_msg_arrival();
static is_first_element();
static fifo_enqueue();
static fifo_empty();
static float expdev();
