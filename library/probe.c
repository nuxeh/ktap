/*
 * probe.c - ktap probing core implementation
 *
 * Copyright (C) 2012 Jovi Zhang <bookjovi@gmail.com>
 *
 * Author: Jovi Zhang <bookjovi@gmail.com>
 *         zhangwei(Jovi) <jovi.zhangwei@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/module.h>
#include <linux/version.h>
#include <linux/perf_event.h>
#include <linux/ftrace_event.h>
#include <linux/kprobes.h>
#include "../ktap.h"

/* this structure allocate on stack */
struct ktap_trace_arg {
	ktap_State *ks;
	Closure *cl;
};

/* this structure allocate on stack */
struct ktap_event {
	struct ftrace_event_call *call;
	void *entry;
	int entry_size;
	struct pt_regs *regs;
	int type;
};

struct ktap_probe_event {
	struct list_head list;
	int type;
	union {
		struct perf_event *perf;
		struct kprobe p;
	} u;
	ktap_State *ks;
	Closure *cl;
	void (*destructor)(struct ktap_probe_event *ktap_pevent);
};

enum {
	EVENT_TYPE_DEFAULT = 0,
	EVENT_TYPE_SYSCALL_ENTER,
	EVENT_TYPE_SYSCALL_EXIT,
	EVENT_TYPE_TRACEPOINT_MAX,
	EVENT_TYPE_KPROBE
};

DEFINE_PER_CPU(bool, ktap_in_tracing);

static void ktap_call_probe_closure(ktap_State *mainthread, Closure *cl,
				    struct ktap_event *e)
{
	ktap_State *ks;
	Tvalue *func;

	ks = ktap_newthread(mainthread);
	setcllvalue(ks->top, cl);
	func = ks->top;
	incr_top(ks);

	if (cl->l.p->numparams) {
		setevalue(ks->top, e);
		incr_top(ks);
	}

	ktap_call(ks, func, 0);
	ktap_exitthread(ks);
}

/* kprobe handler is called in interrupt disabled? */
static int __kprobes pre_handler_kprobe(struct kprobe *p, struct pt_regs *regs)
{
	struct ktap_probe_event *ktap_pevent;
	struct ktap_event e;
	

	if (unlikely(__this_cpu_read(ktap_in_tracing)))
		return 0;

	__this_cpu_write(ktap_in_tracing, true);

	ktap_pevent = container_of(p, struct ktap_probe_event, u.p);

	e.call = NULL;
	e.entry = NULL;
	e.entry_size = 0;
	e.regs = regs;
	e.type = ktap_pevent->type;

	if (same_thread_group(current, G(ktap_pevent->ks)->task))
		goto out;

	ktap_call_probe_closure(ktap_pevent->ks, ktap_pevent->cl, &e);

 out:
	__this_cpu_write(ktap_in_tracing, false);
	return 0;
}


static void kprobe_destructor(struct ktap_probe_event *ktap_pevent)
{
	unregister_kprobe(&ktap_pevent->u.p);
}
static int start_kprobe(ktap_State *ks, const char *event_name, Closure *cl)
{
	struct ktap_probe_event *ktap_pevent;

	ktap_pevent = ktap_zalloc(ks, sizeof(*ktap_pevent));
	ktap_pevent->ks = ks;
	ktap_pevent->cl = cl;
	ktap_pevent->type = EVENT_TYPE_KPROBE;

	INIT_LIST_HEAD(&ktap_pevent->list);
	list_add_tail(&ktap_pevent->list, &G(ks)->probe_events_head);

	ktap_pevent->u.p.symbol_name = event_name;
	ktap_pevent->u.p.pre_handler = pre_handler_kprobe;
	ktap_pevent->u.p.post_handler = NULL;
	ktap_pevent->u.p.fault_handler = NULL;
	ktap_pevent->u.p.break_handler = NULL;
	ktap_pevent->destructor = kprobe_destructor;

	if (register_kprobe(&ktap_pevent->u.p)) {
		ktap_printf(ks, "Cannot register probe: %s\n", event_name);
		list_del(&ktap_pevent->list);
		return -1;
	}

	return 0;
}

static struct trace_iterator *percpu_trace_iterator;
static void event_annotate(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	struct trace_iterator *iter;
	struct trace_event *ev;
	enum print_line_t ret = TRACE_TYPE_NO_CONSUME;

	if (e->type >= EVENT_TYPE_TRACEPOINT_MAX)
		setnilvalue(ra);
		
	/* Simulate the iterator */

	/* iter can be a bit big for the stack, use percpu*/
	iter = per_cpu_ptr(percpu_trace_iterator, smp_processor_id());

	trace_seq_init(&iter->seq);
	iter->ent = e->entry;

	ev = &(e->call->event);
	if (ev)
		ret = ev->funcs->trace(iter, 0, ev);

	if (ret != TRACE_TYPE_NO_CONSUME) {
		struct trace_seq *s = &iter->seq;
		int len = s->len >= PAGE_SIZE ? PAGE_SIZE - 1 : s->len;

		s->buffer[len] = '\0';
		setsvalue(ra, tstring_assemble(ks, s->buffer, len + 1));
	} else
		setnilvalue(ra);
}

static void event_name(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	setsvalue(ra, tstring_new(ks, e->call->name));
}

static void event_print_fmt(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	setsvalue(ra, tstring_new(ks, e->call->print_fmt));
}

/* check pt_regs defintion in linux/arch/x86/include/asm/ptrace.h */
/* support other architecture pt_regs showing */
static void event_regstr(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	struct pt_regs *regs = e->regs;
	char str[256] = {0};

	sprintf(str, "{ax: 0x%lx, orig_ax: 0x%lx, bx: 0x%lx, cx: 0x%lx, dx: 0x%lx, "
		"si: 0x%lx, di: 0x%lx, bp: 0x%lx, ds: 0x%lx, es: 0x%lx, fs: 0x%lx, "
		"gs: 0x%lx, ip: 0x%lx, cs: 0x%lx, flags: 0x%lx, sp: 0x%lx, ss: 0x%lx}\n",
		regs->ax, regs->orig_ax, regs->bx, regs->cx, regs->dx,
		regs->si, regs->di, regs->bp, regs->ds, regs->es, regs->fs,
		regs->gs, regs->ip, regs->cs, regs->flags, regs->sp, regs->ss);
	setsvalue(ra, tstring_new(ks, str));
}

#define ENTRY_HEADSIZE sizeof(struct trace_entry)
struct syscall_trace_enter {
	struct trace_entry      ent;
	int                     nr;
	unsigned long           args[];
};

struct syscall_trace_exit {
	struct trace_entry      ent;
	int                     nr;
	long                    ret;
};

static void event_sc_nr(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	struct syscall_trace_enter *entry = e->entry;

	if (e->type != EVENT_TYPE_SYSCALL_ENTER) {
		setnilvalue(ra);
		return;
	}

	setnvalue(ra, entry->nr);
}

static void event_sc_is_enter(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	struct syscall_trace_enter *entry = e->entry;

	if (e->type == EVENT_TYPE_SYSCALL_ENTER) {
		setbvalue(ra, 1);
	} else {
		setbvalue(ra, 0);
	}
}


#define EVENT_SC_ARGFUNC(n) \
static void event_sc_arg##n(ktap_State *ks, struct ktap_event *e, StkId ra)\
{ \
	struct syscall_trace_enter *entry = e->entry;	\
	if (e->type != EVENT_TYPE_SYSCALL_ENTER) {	\
		setnilvalue(ra);	\
		return;	\
	}	\
	setnvalue(ra, entry->args[n - 1]);	\
}

EVENT_SC_ARGFUNC(1)
EVENT_SC_ARGFUNC(2)
EVENT_SC_ARGFUNC(3)
EVENT_SC_ARGFUNC(4)
EVENT_SC_ARGFUNC(5)
EVENT_SC_ARGFUNC(6)

/***************************/
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0)
struct ftrace_event_field {
	struct list_head        link;
	const char              *name;
	const char              *type;
	int                     filter_type;
	int                     offset;
	int                     size;
	int                     is_signed;
};
#endif

/* e.narg */
static void event_narg(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	setsvalue(ra, tstring_new(ks, e->call->name));
}

static struct list_head *ktap_get_fields(struct ftrace_event_call *event_call)
{
	if (!event_call->class->get_fields)
		return &event_call->class->fields;
	return event_call->class->get_fields(event_call);
}

/* e.allfield */
static void event_allfield(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	char s[128];
	int len, pos = 0;
	struct ftrace_event_field *field;
	struct list_head *head;

	head = ktap_get_fields(e->call);
	list_for_each_entry_reverse(field, head, link) {
		len = sprintf(s + pos, "[%s-%s-%d-%d-%d] ", field->name, field->type,
				 field->offset, field->size, field->is_signed);
		pos += len;
	}
	s[pos] = '\0';

	setsvalue(ra, tstring_new(ks, s));
}

static void event_field(ktap_State *ks, struct ktap_event *e, int index, StkId ra)
{
	struct ftrace_event_field *field;
	struct list_head *head;

	head = ktap_get_fields(e->call);
	list_for_each_entry_reverse(field, head, link) {
		if ((--index == 0) && (field->size == 4)) {
			int n = *(int *)((unsigned char *)e->entry + field->offset);
			setnvalue(ra, n);
			return;
		}
	}

	setnilvalue(ra);
}


static void event_field1(ktap_State *ks, struct ktap_event *e, StkId ra)
{
	event_field(ks, e, 1, ra);
}


#define EVENT_FIELD_BASE	100

static struct event_field_tbl {
	char *name;
	void (*func)(ktap_State *ks, struct ktap_event *e, StkId ra);	
} event_ftbl[] = {
	{"annotate", event_annotate},
	{"name", event_name},
	{"print_fmt", event_print_fmt},
	{"sc_nr", event_sc_nr},
	{"sc_is_enter", event_sc_is_enter},
	{"sc_arg1", event_sc_arg1},
	{"sc_arg2", event_sc_arg2},
	{"sc_arg3", event_sc_arg3},
	{"sc_arg4", event_sc_arg4},
	{"sc_arg5", event_sc_arg5},
	{"sc_arg6", event_sc_arg6},
	{"regstr", event_regstr},
	{"allfield", event_allfield},
	{"field1", event_field1}
};

int ktap_event_get_index(const char *field)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(event_ftbl); i++) {
		if (!strcmp(event_ftbl[i].name, field)) {
			return EVENT_FIELD_BASE + i;
		}
	}

	return -1;
}

Tstring *ktap_event_get_ts(ktap_State *ks, int index)
{
	return tstring_new(ks, event_ftbl[index - EVENT_FIELD_BASE].name);
}

void ktap_event_handle(ktap_State *ks, void *e, int index, StkId ra)
{
	e = (struct ktap_event *)e;

	if (index < EVENT_FIELD_BASE) {
		//event_field(ks, event, index, ra);
	} else
		event_ftbl[index - EVENT_FIELD_BASE].func(ks, e, ra);
}

/* Callback function for perf event subsystem */
static void ktap_overflow_callback(struct perf_event *event,
				   struct perf_sample_data *data,
				   struct pt_regs *regs)
{
	struct ktap_probe_event *ktap_pevent;
	ktap_State  *ks;
	struct ktap_event e;
	unsigned long irq_flags;

	if (unlikely(__this_cpu_read(ktap_in_tracing)))
		return;

	ktap_pevent = event->overflow_handler_context;
	ks = ktap_pevent->ks;

	e.call = event->tp_event;
	e.entry = data->raw->data;
	e.entry_size = data->raw->size;
	e.regs = regs;
	e.type = ktap_pevent->type;

	local_irq_save(irq_flags);
	__this_cpu_write(ktap_in_tracing, true);

	if (same_thread_group(current, G(ks)->task))
		goto out;

	ktap_call_probe_closure(ks, ktap_pevent->cl, &e);

 out:
	__this_cpu_write(ktap_in_tracing, false);
	local_irq_restore(irq_flags);
}

static void perf_destructor(struct ktap_probe_event *ktap_pevent)
{
	perf_event_disable(ktap_pevent->u.perf);
	perf_event_release_kernel(ktap_pevent->u.perf);
}
static void enable_tracepoint_on_cpu(int cpu, struct perf_event_attr *attr,
				     struct ftrace_event_call *call,
				     struct ktap_trace_arg *arg, int type)
{
	struct ktap_probe_event *ktap_pevent;
	struct perf_event *event;

	ktap_pevent = ktap_zalloc(arg->ks, sizeof(*ktap_pevent));
	ktap_pevent->ks = arg->ks;
	ktap_pevent->cl = arg->cl;
	ktap_pevent->type = type;
	ktap_pevent->destructor = perf_destructor;
	event = perf_event_create_kernel_counter(attr, cpu, NULL,
						 ktap_overflow_callback, ktap_pevent);
	if (IS_ERR(event)) {
		int err = PTR_ERR(event);
		ktap_printf(arg->ks, "unable create tracepoint event %s on cpu %d, err: %d\n",
				call->name, cpu, err);
		ktap_free(arg->ks, ktap_pevent);
		return;
	}

	ktap_pevent->u.perf = event;
	INIT_LIST_HEAD(&ktap_pevent->list);
	list_add_tail(&ktap_pevent->list, &G(arg->ks)->probe_events_head);

	perf_event_enable(event);
}

static void enable_tracepoint(struct ftrace_event_call *call, void *data)
{
	struct ktap_trace_arg *arg = data;
	struct perf_event_attr attr;
	int cpu, type = EVENT_TYPE_DEFAULT;

	ktap_printf(arg->ks, "enable tracepoint event: %s\n", call->name);

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_TRACEPOINT;	
	attr.config = call->event.type;
	attr.sample_type = PERF_SAMPLE_RAW | PERF_SAMPLE_TIME |
			   PERF_SAMPLE_CPU | PERF_SAMPLE_PERIOD;
	attr.sample_period = 1;
	attr.size = sizeof(attr);

	if (!strncmp(call->name, "sys_enter_", 10)) {
		type = EVENT_TYPE_SYSCALL_ENTER;
	} else if (!strncmp(call->name, "sys_exit_", 9)) {
		type = EVENT_TYPE_SYSCALL_EXIT;
	}

	for_each_possible_cpu(cpu)
		enable_tracepoint_on_cpu(cpu, &attr, call, arg, type);
}

struct list_head *ftrace_events_ptr;

typedef void (*ftrace_call_func)(struct ftrace_event_call * call, void *data);
/* helper function for ktap register tracepoint */
static void ftrace_on_event_call(const char *buf, ftrace_call_func actor,
				 void *data)
{
	char *event = NULL, *sub = NULL, *match, *buf_ptr = NULL;
	char new_buf[32] = {0};
	struct ftrace_event_call *call;

	if (buf) {
		/* argument buf is const, so we need to prepare a changeable buff */
		strncpy(new_buf, buf, 31);
		buf_ptr = new_buf;
	}

	/*
	 * The buf format can be <subsystem>:<event-name>
	 *  *:<event-name> means any event by that name.
	 *  :<event-name> is the same.
	 *
	 *  <subsystem>:* means all events in that subsystem
	 *  <subsystem>: means the same.
	 *
	 *  <name> (no ':') means all events in a subsystem with
	 *  the name <name> or any event that matches <name>
	 */

	match = strsep(&buf_ptr, ":");
	if (buf_ptr) {
		sub = match;
		event = buf_ptr;
		match = NULL;

		if (!strlen(sub) || strcmp(sub, "*") == 0)
			sub = NULL;
		if (!strlen(event) || strcmp(event, "*") == 0)
			event = NULL;
	}

	list_for_each_entry(call, ftrace_events_ptr, list) {

		if (!call->name || !call->class || !call->class->reg)
			continue;

		if (call->flags & TRACE_EVENT_FL_IGNORE_ENABLE)
			continue;

		if (match &&
		    strcmp(match, call->name) != 0 &&
		    strcmp(match, call->class->system) != 0)
			continue;

		if (sub && strcmp(sub, call->class->system) != 0)
			continue;

		if (event && strcmp(event, call->name) != 0)
			continue;

		(*actor)(call, data);
	}
}

static int start_tracepoint(ktap_State *ks, const char *event_name, Closure *cl)
{
	struct ktap_trace_arg arg;

	if (*event_name == '\0')
		event_name = NULL;

	arg.ks = ks;
	arg.cl = cl;
	ftrace_on_event_call(event_name, enable_tracepoint, (void *)&arg);
	return 0;
}

int start_probe(ktap_State *ks, const char *event_name, Closure *cl)
{
	if (!strncmp(event_name, "kprobe:", 7)) {
		return start_kprobe(ks, event_name + 7, cl);
	} else if (!strncmp(event_name, "kprobes:", 8)) {
		return start_kprobe(ks, event_name + 8, cl);
	} else if (!strncmp(event_name, "tracepoint:", 11)) {
		return start_tracepoint(ks, event_name + 11, cl);
	} else if (!strncmp(event_name, "tp:", 3)) {
		return start_tracepoint(ks, event_name + 3, cl);
	} else {
		ktap_printf(ks, "unknown probe event name: %s\n", event_name);
		return -1;
	}

}

void end_probes(struct ktap_State *ks)
{
	struct ktap_probe_event *ktap_pevent;
	struct list_head *tmp, *pos;
	struct list_head *head = &G(ks)->probe_events_head;

	list_for_each(pos, head) {
		ktap_pevent = container_of(pos, struct ktap_probe_event,
					   list);
		ktap_pevent->destructor(ktap_pevent);
        }
       	/*
	 * Ensure our callback won't be called anymore. The buffers
	 * will be freed after that.
	 */
       	tracepoint_synchronize_unregister();

	list_for_each_safe(pos, tmp, head) {
		ktap_pevent = container_of(pos, struct ktap_probe_event,
					   list);
		list_del(&ktap_pevent->list);
		ktap_free(ks, ktap_pevent);
	}
}

void ktap_probe_exit(ktap_State *ks)
{
	end_probes(ks);

	if (!G(ks)->trace_enabled)
		return;

	free_percpu(percpu_trace_iterator);

	G(ks)->trace_enabled = 0;
}

int ktap_probe_init(ktap_State *ks)
{
	INIT_LIST_HEAD(&(G(ks)->probe_events_head));

	/* allocate percpu data */
	if (!G(ks)->trace_enabled) {
		percpu_trace_iterator = alloc_percpu(struct trace_iterator);
		if (!percpu_trace_iterator)
			return -1;

		G(ks)->trace_enabled = 1;
	}

	/* get ftrace_events global variable if ftrace_events not exported */
	ftrace_events_ptr = kallsyms_lookup_name("ftrace_events");
	if (!ftrace_events_ptr) {
		G(ks)->trace_enabled = 0;
		free_percpu(percpu_trace_iterator);
		ktap_printf(ks, "cannot lookup ftrace_events in kallsyms\n");
		return -1;
	}

	return 0;
}