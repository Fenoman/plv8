/*-------------------------------------------------------------------------
 *
 * plv8.cc : PL/v8 handler routines.
 *
 * Copyright (c) 2009-2012, the PLV8JS Development Group.
 *-------------------------------------------------------------------------
 */
#include "plv8.h"

#ifdef _MSC_VER
#undef open
#endif

#include "libplatform/libplatform.h"
#include "plv8_allocator.h"


extern "C" {
#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_database.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "common/hashfn.h"
#include "executor/spi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timeout.h"
#include "utils/timestamp.h"
#if PG_VERSION_NUM >= 190000
#include "utils/hsearch.h"
#endif

#include <exception>
#ifndef WIN32
#include <pthread.h>
#include <signal.h>
#endif

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#if PG_VERSION_NUM >= 180000
PG_MODULE_MAGIC_EXT(
	.name = "plv8",
	.version = PLV8_VERSION
);
#else
PG_MODULE_MAGIC;
#endif

PGDLLEXPORT Datum	plv8_call_handler(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_call_validator(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_reset(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum	plv8_info(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_call_handler);
PG_FUNCTION_INFO_V1(plv8_call_validator);
PG_FUNCTION_INFO_V1(plv8_reset);
PG_FUNCTION_INFO_V1(plv8_info);


PGDLLEXPORT void _PG_init(void);

PGDLLEXPORT Datum	plv8_inline_handler(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(plv8_inline_handler);
} // extern "C"

using namespace v8;

/*
 * Tantor uses 64-bit xids and keeps xmin in HeapTupleData, where
 * HeapTupleGetXmin() reads it. Community PostgreSQL reads it from the tuple
 * header with HeapTupleHeaderGetXmin(). Tantor 17 defines the accessor as a
 * macro and Tantor 18 as a static inline function, so the macro test alone
 * misses Tantor 18. TANTOR_VERSION comes from pg_config.h and is set by every
 * Tantor edition, unlike the -DTANTOR_SE and -DTANTOR_SE1C edition flags.
 */
static inline TransactionId
plv8_heap_tuple_get_xmin(HeapTuple tuple)
{
#if defined(HeapTupleGetXmin) || defined(TANTOR_VERSION)
	return HeapTupleGetXmin(tuple);
#else
	return HeapTupleHeaderGetXmin(tuple->t_data);
#endif
}

typedef struct plv8_proc_cache
{
	Oid						fn_oid;

	Persistent<Function>	function;
	char					proname[NAMEDATALEN];
	char				   *prosrc;

	TransactionId			fn_xmin;
	ItemPointerData			fn_tid;
	Oid						user_id;

	int						nargs;
	bool					retset;		/* true if SRF */
	Oid						rettype;
	Oid						argtypes[FUNC_MAX_ARGS];
} plv8_proc_cache;

plv8_context *current_context = nullptr;
ErrorData *plv8_pending_cancel = nullptr;
static uint64 next_context_id = 1;
size_t plv8_memory_limit = 0;
size_t plv8_last_heap_size = 0;

/*
 * The function and context are created at the first invocation.  Their
 * lifetime is same as plv8_proc, but they are not palloc'ed memory,
 * so we need to clear them at the end of transaction.
 */
typedef struct plv8_exec_env
{
	Isolate 			   *isolate;
	Persistent<Object>		recv;
	Persistent<Context>		context;
	Local<Context> localContext() { return Local<Context>::New(isolate, context) ; }
	uint64					context_id;	/* plv8_context.id this env was created for */
	struct plv8_exec_env   *next;
} plv8_exec_env;

/*
 * We cannot cache plv8_type inter executions because it has FmgrInfo fields.
 * So, we cache rettype and argtype in fn_extra only during one execution.
 */
typedef struct plv8_proc
{
	plv8_proc_cache		   *cache;
	plv8_exec_env		   *xenv;
	TypeFuncClass			functypclass;			/* For SRF */
	plv8_type				rettype;
	plv8_type				argtypes[FUNC_MAX_ARGS];
} plv8_proc;

static HTAB *plv8_proc_cache_hash = NULL;

static plv8_exec_env		   *exec_env_head = NULL;

static void killPlv8Context(plv8_context *ctx);

/*
 * lower_case_functions are postgres-like C functions.
 * They could raise errors with elog/ereport(ERROR).
 */
static plv8_proc *plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo,
		bool validate, char ***argnames) throw();
static void plv8_xact_cb(XactEvent event, void *arg);

/*
 * CamelCaseFunctions are C++ functions.
 * They could raise errors with C++ throw statements, or never throw exceptions.
 */
static plv8_exec_env *CreateExecEnv(Handle<Function> function, plv8_context *context);
static plv8_exec_env *CreateExecEnv(Persistent<Function>& function, plv8_context *context);
static plv8_proc *Compile(Oid fn_oid, FunctionCallInfo fcinfo,
					bool validate, bool is_trigger);
static Local<Function> CompileFunction(plv8_context *global_context,
					const char *proname, int proarglen,
					const char *proargs[], const char *prosrc,
					bool is_trigger, bool retset);
static Datum CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
		int nargs, plv8_type argtypes[], plv8_type *rettype);
static Datum CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv);
static plv8_context *GetPlv8Context();
static Local<ObjectTemplate> GetGlobalObjectTemplate(Isolate *isolate);
static void CreateIsolate(plv8_context *context);

/* A GUC to specify a custom start up function to call */
static char *plv8_start_proc = NULL;

/* A GUC to specify V8 flags (e.g. --es_staging) */
static char *plv8_v8_flags = NULL;

/* A GUC to specify the ICU data directory */
static char *plv8_icu_data = NULL;

/* A GUC to specify the remote debugger port */
static int plv8_debugger_port;

#ifdef EXECUTION_TIMEOUT
static int plv8_execution_timeout = 300;
#endif

static std::unique_ptr<v8::Platform> v8_platform = NULL;

/*
 * We use vector instead of hash since the size of this array
 * is expected to be short in most cases.
 */
static std::vector<plv8_context *> ContextVector;

#ifdef ENABLE_DEBUGGER_SUPPORT
v8::Persistent<v8::Context> debug_message_context;

void DispatchDebugMessages() {
  // We are in some random thread. We should already have v8::Locker acquired
  // (we requested this when registered this callback). We was called
  // because new debug messages arrived; they may have already been processed,
  // but we shouldn't worry about this.
  //
  // All we have to do is to set context and call ProcessDebugMessages.
  //
  // We should decide which V8 context to use here. This is important for
  // "evaluate" command, because it must be executed some context.
  // In our sample we have only one context, so there is nothing really to
  // think about.
  v8::Context::Scope scope(debug_message_context);

  v8::Debug::ProcessDebugMessages();
}
#endif  // ENABLE_DEBUGGER_SUPPORT

void OOMErrorHandler(const char* location, const v8::OOMDetails &) {
	Isolate *isolate = Isolate::GetCurrent();
	isolate->TerminateExecution();
	// set it to kill the user context and isolate
	current_context->is_dead = true;
	elog(ERROR, "Out of memory error");
}

void GCEpilogueCallback(Isolate* isolate, GCType type, GCCallbackFlags /* flags */) {
	HeapStatistics heap_statistics;
	isolate->GetHeapStatistics(&heap_statistics);
	if (type != GCType::kGCTypeIncrementalMarking
		&& heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB) {
		const char *error_message = "Out of memory";
		Local<v8::String>	result = ToString(error_message, 13);
		isolate->ThrowException(result);

		isolate->TerminateExecution();
	}
	if (heap_statistics.used_heap_size() > plv8_memory_limit * 1_MB / 0.9
		&& plv8_last_heap_size < plv8_memory_limit * 1_MB / 0.9) {
		isolate->LowMemoryNotification();
	}
	plv8_last_heap_size = heap_statistics.used_heap_size();
}

size_t NearHeapLimitHandler(void* data, size_t current_heap_limit,
								size_t initial_heap_limit) {
	Isolate *isolate = Isolate::GetCurrent();
	isolate->TerminateExecution();
	// need to give back more space
	// to make sure it can unwind the stack and process exceptions
	return current_heap_limit + 1_MB;
}

void PromiseRejectCB(PromiseRejectMessage rejection) {
	auto event = rejection.GetEvent();
	if (event == kPromiseRejectAfterResolved || event == kPromiseResolveAfterResolved)
		return;
	auto	promise = rejection.GetPromise();
	auto	isolate = promise->GetIsolate();

	if (rejection.GetEvent() == v8::kPromiseHandlerAddedAfterReject) {
		if (current_context->ignore_unhandled_promises) return;
		// Remove handled promises from the list
		auto& unhandled_promises = current_context->unhandled_promises;
		for (auto it = unhandled_promises.begin(); it != unhandled_promises.end(); ++it) {
			auto unhandled_promise = std::get<0>(*it).Get(isolate);
			if (unhandled_promise == promise) {
				unhandled_promises.erase(it--);
			}
		}
		return;
	}

	auto exception = rejection.GetValue();
	Local<Message> message;
	// Assume that all objects are stack-traces.
	if (exception->IsObject()) {
		message = Exception::CreateMessage(isolate, exception);
	}
	if (!exception->IsNativeError() &&
		(message.IsEmpty() || message->GetStackTrace().IsEmpty())) {
		// If there is no real Error object, manually throw and catch a stack trace.
		TryCatch try_catch(isolate);
		try_catch.SetVerbose(true);
		isolate->ThrowException(Exception::Error(
				v8::String::NewFromUtf8Literal(isolate, "Unhandled Promise.")));
		message = try_catch.Message();
		exception = try_catch.Exception();
	}
	if (current_context->ignore_unhandled_promises) return;
	current_context->unhandled_promises.emplace_back(
			v8::Global<v8::Promise>(isolate, promise),
			v8::Global<v8::Message>(isolate, message),
			v8::Global<v8::Value>(isolate, exception));
}

void HandleUnhandledPromiseRejections() {
	// Avoid recursive calls to HandleUnhandledPromiseRejections.
	auto isolate = current_context->isolate;
	if (current_context->ignore_unhandled_promises) return;
	current_context->ignore_unhandled_promises = true;
	HandleScope scope(isolate);
	// Ignore promises that get added during error reporting.
	size_t i = 0;
	auto& unhandled_promises = current_context->unhandled_promises;
	for (; i < unhandled_promises.size(); i++) {
		const auto& tuple = unhandled_promises[i];
		Local<v8::Message> message = std::get<1>(tuple).Get(isolate);
		Local<v8::Value> exception = std::get<2>(tuple).Get(isolate);
		js_error error(isolate, exception, message);
		error.log(WARNING, "Unhandled Promise rejection: %s");
	}
	unhandled_promises.clear();
	current_context->ignore_unhandled_promises = false;
}

static void
CreateIsolate(plv8_context *context) {
	Isolate *isolate;
	Isolate::CreateParams params;
	params.array_buffer_allocator = new ArrayAllocator(plv8_memory_limit * 1_MB);
	//params.array_buffer_allocator =
  //    v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	ResourceConstraints rc;
	rc.ConfigureDefaults(plv8_memory_limit * 1_MB * 2, plv8_memory_limit * 1_MB * 2);
	params.constraints = rc;
	isolate = Isolate::New(params);
	isolate->SetOOMErrorHandler(OOMErrorHandler);
	isolate->AddGCEpilogueCallback(GCEpilogueCallback);
	isolate->AddNearHeapLimitCallback(NearHeapLimitHandler, NULL);
	isolate->SetPromiseRejectCallback(PromiseRejectCB);
	context->isolate = isolate;
	context->array_buffer_allocator = params.array_buffer_allocator;
}

void
_PG_init(void)
{
	HASHCTL    hash_ctl = { 0 };

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(plv8_proc_cache);
	hash_ctl.hash = oid_hash;
	plv8_proc_cache_hash = hash_create("PLv8 Procedures", 32,
									   &hash_ctl, HASH_ELEM | HASH_FUNCTION);

    config_generic *guc_value;

#define START_PROC_VAR "plv8.start_proc"
    guc_value = plv8_find_option(START_PROC_VAR);
    if (guc_value != NULL) {
        plv8_start_proc = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(START_PROC_VAR,
                                   gettext_noop("PLV8 function to run once when PLV8 is first used."),
                                   NULL,
                                   &plv8_start_proc,
                                   NULL,
                                   PGC_USERSET, 0,
                                   NULL,
                                   NULL,
                                   NULL);
    }
#undef START_PROC_VAR

#define ICU_DATA_VAR "plv8.icu_data"
    guc_value = plv8_find_option(ICU_DATA_VAR);
    if (guc_value != NULL) {
        plv8_icu_data = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(ICU_DATA_VAR,
                                   gettext_noop("ICU data file directory."),
                                   NULL,
                                   &plv8_icu_data,
                                   NULL,
                                   PGC_USERSET, 0,
                                   NULL,
                                   NULL,
                                   NULL);
    }
#undef ICU_DATA_VAR

#define V8_FLAGS_VAR "plv8.v8_flags"
    guc_value = plv8_find_option(V8_FLAGS_VAR);
    if (guc_value != NULL) {
        plv8_v8_flags = plv8_string_option(guc_value);
    } else {
        DefineCustomStringVariable(V8_FLAGS_VAR,
                                   gettext_noop("V8 engine initialization flags (e.g. --harmony for all current harmony features)."),
                                   NULL,
                                   &plv8_v8_flags,
                                   NULL,
                                   PGC_USERSET, 0,
                                   NULL,
                                   NULL,
                                   NULL);
    }
#undef V8_FLAGS_VAR

#define DEBUGGER_PORT_VAR "plv8.debugger_port"
    guc_value = plv8_find_option(DEBUGGER_PORT_VAR);
    if (guc_value != NULL) {
        plv8_debugger_port = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(DEBUGGER_PORT_VAR,
                                gettext_noop("V8 remote debug port."),
                                gettext_noop("The default value is 35432.  "
                                             "This is effective only if PLV8 is built with ENABLE_DEBUGGER_SUPPORT."),
                                &plv8_debugger_port,
                                35432, 0, 65536,
                                PGC_USERSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef DEBUGGER_PORT_VAR

#ifdef EXECUTION_TIMEOUT
#define EXECUTION_TIMEOUT_VAR "plv8.execution_timeout"
	guc_value = plv8_find_option(EXECUTION_TIMEOUT_VAR);
	if (guc_value != NULL) {
		plv8_execution_timeout = plv8_int_option(guc_value);
	} else {
        DefineCustomIntVariable(EXECUTION_TIMEOUT_VAR,
                                gettext_noop("V8 execution timeout."),
                                gettext_noop("The default value is 300 seconds.  "
                                             "This allows you to override the default execution timeout."),
                                &plv8_execution_timeout,
                                300, 1, 65536,
                                PGC_USERSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef EXECUTION_TIMEOUT_VAR
#endif

#define MEMORY_LIMIT_VAR "plv8.memory_limit"
    guc_value = plv8_find_option(MEMORY_LIMIT_VAR);
    if (guc_value != NULL) {
        plv8_memory_limit = plv8_int_option(guc_value);
    } else {
        DefineCustomIntVariable(MEMORY_LIMIT_VAR,
                                gettext_noop("Per-isolate memory limit in MBytes"),
                                gettext_noop("The default value is 256 MB"),
                                (int *) &plv8_memory_limit,
                                256, 256, 3096, // hardcoded v8 limits for isolates
                                PGC_SUSET, 0,
                                NULL,
                                NULL,
                                NULL);
    }
#undef MEMORY_LIMIT_VAR

	RegisterXactCallback(plv8_xact_cb, NULL);

	EmitWarningsOnPlaceholders("plv8");

	if (plv8_icu_data == NULL) {
		elog(DEBUG1, "no icu dir");
		V8::InitializeICU();
	} else {
		elog(DEBUG1, "init icu data %s", plv8_icu_data);
		V8::InitializeICU(plv8_icu_data);
	}

#if (V8_MAJOR_VERSION == 4 && V8_MINOR_VERSION >= 6) || V8_MAJOR_VERSION >= 5
	V8::InitializeExternalStartupData("plv8");
#endif
	if (!v8_platform) {
#ifndef WIN32
		/*
		 * The worker threads inherit the signal mask.  They start with all
		 * signals blocked, so the handlers of PostgreSQL, the interrupt
		 * check among them, run on the backend thread only.
		 */
		sigset_t	block_all, saved;

		sigfillset(&block_all);
		pthread_sigmask(SIG_SETMASK, &block_all, &saved);
		v8_platform = platform::NewDefaultPlatform();
		pthread_sigmask(SIG_SETMASK, &saved, NULL);
#else
		v8_platform = platform::NewDefaultPlatform();
#endif
	}

	if (plv8_v8_flags != NULL) {
		V8::SetFlagsFromString(plv8_v8_flags);
	}

	V8::InitializePlatform(v8_platform.get());

	V8::Initialize();
}

/*
 * Interrupting JavaScript.
 *
 * Query cancel, statement_timeout and pg_terminate_backend() only set flags
 * that CHECK_FOR_INTERRUPTS() reads, and JavaScript never gets there.  While
 * plv8 runs, a PostgreSQL timeout checks the flags every 25 ms and stops
 * V8.  The code that sees the stopped script then lets PostgreSQL report the
 * interrupt (plv8_ReportInterrupt()).
 *
 * The timeout handler runs from SIGALRM on the backend thread (the V8 worker
 * threads block signals).  It reads flags and current_context, and
 * TerminateExecution() takes the recursive mutex of the isolate, as the
 * signal handler that plv8 used before did.  A request lost to a race with
 * V8 is made again by the next check.
 */
#define PLV8_INTERRUPT_CHECK_MS		25

static TimeoutId plv8_interrupt_timeout = MAX_TIMEOUTS;

#if PG_VERSION_NUM >= 150000
static void
plv8_interrupt_check(void)
{
	plv8_context   *context = current_context;

	/*
	 * What ProcessInterrupts() acts on.  InterruptHoldoffCount is not
	 * checked: handle_sig_alarm() holds interrupts while it runs handlers.
	 */
	if (context == NULL)
		return;
	if ((QueryCancelPending && QueryCancelHoldoffCount == 0) || ProcDiePending
#if PG_VERSION_NUM >= 170000
		|| TransactionTimeoutPending
#endif
		)
	{
		context->interrupted = true;
		context->isolate->TerminateExecution();
	}
}
#endif

static bool
plv8_InterruptActive()
{
	return plv8_interrupt_timeout != MAX_TIMEOUTS &&
		get_timeout_active(plv8_interrupt_timeout);
}

static void
plv8_InterruptDisarm()
{
	if (plv8_interrupt_timeout != MAX_TIMEOUTS)
		disable_timeout(plv8_interrupt_timeout, false);
}

/*
 * Keeps the interrupt check on while plv8 runs.
 *
 * Only the outermost scope arms the timeout, nested ones see it active.  A
 * PostgreSQL error that skips the destructor goes through the PG_CATCH of
 * CallFunction(), CallSRFunction() or CallTrigger(), which disarms what the
 * call armed.
 *
 * PostgreSQL before 15 has no repeating timeout, there the check is off.
 */
class Plv8InterruptScope
{
private:
	bool		m_armed;

public:
	explicit Plv8InterruptScope(plv8_context *context) : m_armed(false)
	{
		/*
		 * A call starts clean, as DoCall() does: a stop left in the isolate
		 * is stale.  No call starts inside a script that is being stopped,
		 * plv8_FunctionInvoker() runs no callback for it.
		 */
		if (context->isolate->IsExecutionTerminating())
			context->isolate->CancelTerminateExecution();
		context->interrupted = false;

#if PG_VERSION_NUM >= 150000
		if (plv8_interrupt_timeout == MAX_TIMEOUTS)
			plv8_interrupt_timeout = RegisterTimeout(USER_TIMEOUT, plv8_interrupt_check);
		if (get_timeout_active(plv8_interrupt_timeout))
			return;

		enable_timeout_every(plv8_interrupt_timeout,
							 TimestampTzPlusMilliseconds(GetCurrentTimestamp(),
														 PLV8_INTERRUPT_CHECK_MS),
							 PLV8_INTERRUPT_CHECK_MS);
		m_armed = true;
#endif
	}

	~Plv8InterruptScope()
	{
		if (m_armed)
			plv8_InterruptDisarm();
	}
};

/*
 * Report a script stopped by an interrupt.
 *
 * PostgreSQL reports the cancel or termination it has pending.  A query
 * cancel that the script got from a SPI call (plv8_pending_cancel) is raised
 * again, the script could not catch it.
 */
void
plv8_ReportInterrupt()
{
	ErrorData  *cancel = plv8_pending_cancel;

	plv8_pending_cancel = nullptr;
	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
		if (cancel != nullptr)
			ReThrowError(cancel);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	throw js_error("Signal caught: interrupted");
}

static void
plv8_xact_cb(XactEvent event, void *arg)
{
	plv8_exec_env	   *env = exec_env_head;

	/* it is in TopTransactionContext */
	plv8_pending_cancel = nullptr;

	while (env)
	{
		if (!env->recv.IsEmpty())
		{
			env->recv.Reset();
		}
		env = env->next;
		/*
		 * Each item was allocated in TopTransactionContext, so
		 * it will be freed eventually.
		 */
	}
	exec_env_head = NULL;
}

static inline plv8_exec_env *
plv8_new_exec_env(plv8_context *context)
{
	plv8_exec_env	   *xenv = (plv8_exec_env *)
		MemoryContextAllocZero(TopTransactionContext, sizeof(plv8_exec_env));

	new(&xenv->context) Persistent<Context>();
	new(&xenv->recv) Persistent<Object>();
	xenv->isolate = context->isolate;
	xenv->context_id = context->id;

	/*
	 * Add it to the list, which will be freed in the end of top transaction.
	 */
	xenv->next = exec_env_head;
	exec_env_head = xenv;

	return xenv;
}

/*
 * GetPlv8Context() for an entry point outside its C++ try block.  Making a
 * context runs plv8.start_proc, and its errors and interrupts are C++
 * exceptions.
 */
static plv8_context *
GetPlv8ContextOrError()
{
	try
	{
		return GetPlv8Context();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return nullptr;	// keep compiler quiet
}

Datum
plv8_call_handler(PG_FUNCTION_ARGS)
{
	plv8_context   *context = GetPlv8ContextOrError();
	Oid		fn_oid = fcinfo->flinfo->fn_oid;
	bool	is_trigger = CALLED_AS_TRIGGER(fcinfo);

	try
	{
		CurrentContextScope	context_scope(context);
		Isolate::Scope	scope(context->isolate);
		HandleScope	handle_scope(context->isolate);
		plv8_proc	   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		/*
		 * The proc cached in fn_extra carries an execution environment that
		 * was created for the context of whoever first called the function
		 * through this FmgrInfo.  If the effective user has changed since
		 * (a SECURITY DEFINER routine, SET ROLE) or that context is gone
		 * (plv8_reset(), an isolate killed after running out of memory),
		 * it refers to a different isolate than the one we just entered.
		 * Running it there mixes isolates and crashes V8 (issue #606), so
		 * rebuild it for the current context instead.  This is the same
		 * thing that happens on the first call under a new user, when
		 * plv8_get_proc() notices the user change and recompiles.
		 */
		if (proc && proc->xenv->context_id != context->id)
		{
			pfree(proc);
			proc = NULL;
			fcinfo->flinfo->fn_extra = NULL;
		}

		if (!proc)
		{
			proc = Compile(fn_oid, fcinfo, false, is_trigger);
			proc->xenv = CreateExecEnv(proc->cache->function, context);
			fcinfo->flinfo->fn_extra = proc;
		}

		plv8_proc_cache *cache = proc->cache;

		if (is_trigger)
			return CallTrigger(fcinfo, proc->xenv);
		else if (cache->retset)
			return CallSRFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
		else
			return CallFunction(fcinfo, proc->xenv,
						cache->nargs, proc->argtypes, &proc->rettype);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}


static void killPlv8Context(plv8_context *ctx) {
	HASH_SEQ_STATUS		status;
	plv8_proc_cache*	cache;

	// need to search and reset all the user functions which were created in the old context
	hash_seq_init(&status, plv8_proc_cache_hash);
	cache = (plv8_proc_cache *) hash_seq_search(&status);
	while (cache != nullptr) {
		if (cache->user_id == ctx->user_id) {
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
		}
		cache = (plv8_proc_cache *) hash_seq_search(&status);
	}

	/*
	 * Execution environments created for this context hold global handles
	 * in the isolate we are about to dispose of.  Release them now, or
	 * plv8_xact_cb would try to at the end of the transaction, on a dead
	 * isolate.  The call handler will notice the context id mismatch and
	 * rebuild any of them that is still referenced from a fn_extra.
	 */
	for (plv8_exec_env *env = exec_env_head; env != NULL; env = env->next)
	{
		if (env->context_id == ctx->id)
		{
			env->recv.Reset();
			env->context.Reset();
		}
	}

	ctx->isolate->Dispose();
	delete ctx->array_buffer_allocator;
}

Datum
plv8_reset(PG_FUNCTION_ARGS)
{
	Oid					user_id = GetUserId();
	unsigned long		i;

	for (i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->user_id == user_id)
		{
			plv8_context * context = ContextVector[i];
			ContextVector.erase(ContextVector.begin() + i);
			killPlv8Context(context);
			pfree(context);
			break;
		}
	}
	return (Datum) 0;
}

Datum
plv8_info(PG_FUNCTION_ARGS)
{
	unsigned long		i;
	unsigned long 		size = ContextVector.size();
	char 				**infos = nullptr;
	size_t 				*lengths = nullptr;
	size_t 				total_length = 3; // length of "[]\0"

	MemoryContext execution_context, old_context;

	if (size == 0)
	{
		PG_RETURN_NULL();
	}

  execution_context = AllocSetContextCreate(CurrentMemoryContext,
                                          "plv8_info Context",
                                          ALLOCSET_SMALL_SIZES);

	old_context = MemoryContextSwitchTo(execution_context);

	infos = (char **) palloc(sizeof(char *) * size);
	lengths = (size_t *) palloc(sizeof(size_t) * size);

	/*
	 * JSON and its methods belong to the scripts of each context and can
	 * throw.  An error is raised after the loop, once the V8 scopes of the
	 * context are closed.
	 */
	js_error	js_err;
	bool		js_failed = false;
	bool		pg_failed = false;

	for (i = 0; i < size && !js_failed && !pg_failed; i++)
	{
		Isolate 	   	   *isolate = ContextVector[i]->isolate;
		Isolate::Scope		scope(isolate);
		HandleScope			handle_scope(isolate);
		Local<Context>		context = ContextVector[i]->localContext();
		Context::Scope		context_scope(context);
		CurrentContextScope	current_scope(ContextVector[i]);

		/* no script runs now, a stop left in the isolate is stale */
		if (isolate->IsExecutionTerminating())
			isolate->CancelTerminateExecution();
		ContextVector[i]->interrupted = false;

		try
		{
			JSONObject 			JSON;
			Local<v8::Value>	result;
			Local<v8::Object>   obj = v8::Object::New(isolate);

			/* without an error, the role of an old context can be dropped */
			char 			   *username = GetUserNameFromId(ContextVector[i]->user_id, true);

			obj->CreateDataProperty(context, v8::String::NewFromUtf8Literal(isolate, "user"),
			   v8::String::NewFromUtf8(isolate, username ? username : "?").ToLocalChecked()).Check();
			GetMemoryInfo(obj);

			result = JSON.Stringify(obj);
			CString str(plv8_ToStringChecked(isolate, result));

			infos[i] = pstrdup(str.str());
			lengths[i] = strlen(infos[i]);
			total_length += lengths[i] + 1; // add 1 byte for ','
		}
		catch (js_error &e) { js_err = e; js_failed = true; }
		catch (pg_error &e) { pg_failed = true; }
	}

	if (js_failed || pg_failed)
	{
		MemoryContextSwitchTo(old_context);
		if (js_failed)
			js_err.rethrow();
		pg_error().rethrow();
	}

	char *out = (char *) palloc0(total_length);
	out[0] = '[';
	size_t current = 0;
	for (i = 0; i < size; i++)
	{
		++current;
		strcpy(out + current, infos[i]);
		current += lengths[i];
		if (i != size - 1)
		{
			out[current] = ',';
		}
	}

	out[current] = ']';
	out[current + 1] = '\0';

	MemoryContextSwitchTo(old_context);

	return CStringGetTextDatum(out);
}

Datum
plv8_inline_handler(PG_FUNCTION_ARGS)
{
	InlineCodeBlock *codeblock = (InlineCodeBlock *) DatumGetPointer(PG_GETARG_DATUM(0));

	Assert(IsA(codeblock, InlineCodeBlock));

	try
	{
		plv8_context	   *context = GetPlv8Context();
		CurrentContextScope	context_scope(context);
		Isolate::Scope		scope(context->isolate);
		HandleScope			handle_scope(context->isolate);
		char			   *source_text = codeblock->source_text;

		Local<Function>	function = CompileFunction(context,
										NULL, 0, NULL,
										source_text, false, false);
		plv8_exec_env	   *xenv = CreateExecEnv(function, context);
		return CallFunction(fcinfo, xenv, 0, NULL, NULL);
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

#ifdef EXECUTION_TIMEOUT
/*
 * Breakout -- break out of a Call, with a thread
 *
 * This function breaks out of a javascript execution context.
 */
#ifdef _MSC_VER // windows
DWORD WINAPI
Breakout (LPVOID lpParam)
{
	Isolate *isolate = (Isolate *) lpParam;
	Sleep(plv8_execution_timeout * 1000);
	isolate->TerminateExecution();

	return 0;
}
#else // posix
void *
Breakout (void *d)
{
	auto *isolate = (Isolate *) d;
	sleep(plv8_execution_timeout);
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
	isolate->TerminateExecution();

	return NULL;
}
#endif
#endif

/*
 * DoCall -- Call a JS function with SPI support.
 *
 * This function could throw C++ exceptions, but must not throw PG exceptions.
 */
static Local<v8::Value>
DoCall(Local<Context> ctx, Handle<Function> fn, Handle<Object> receiver,
	int nargs, Handle<v8::Value> args[], bool nonatomic)
{
	Isolate 	   *isolate = ctx->GetIsolate();
	TryCatch		try_catch(isolate);

	if (isolate->IsExecutionTerminating() || current_context->interrupted) {
		isolate->CancelTerminateExecution();
		if (current_context->interrupted) {
			current_context->interrupted = false;
		}
	}
	if (SPI_connect_ext(nonatomic ? SPI_OPT_NONATOMIC : 0) != SPI_OK_CONNECT)
		throw js_error("could not connect to SPI manager");

#ifdef EXECUTION_TIMEOUT
	bool timeout = false;
#ifdef _MSC_VER // windows
	HANDLE  hThread;
	DWORD dwThreadId;
	hThread = CreateThread(NULL, 0, Breakout, isolate, 0, &dwThreadId);
#else
	pthread_t breakout_thread;
	// set up the thread to break out the execution if needed
	pthread_create(&breakout_thread, NULL, Breakout, isolate);
#endif
#endif

	try {
	MaybeLocal<v8::Value> result;
	{
		Plv8InterruptScope	interrupt_scope(current_context);

		result = fn->Call(ctx, receiver, nargs, args);
	}

	int	status = SPI_finish();

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
	if (TerminateThread(hThread, NULL) == 0) {
		timeout = true;
	}
#else
	void *thread_result;
	pthread_cancel(breakout_thread);
	pthread_join(breakout_thread, &thread_result);
	if (thread_result == NULL) {
		timeout = true;
	}
#endif
#endif

	/* An interrupt stops the call even when the script managed to return. */
	if (current_context->interrupted) {
		isolate->CancelTerminateExecution();
		current_context->interrupted = false;
		plv8_ReportInterrupt();
	}

	HandleUnhandledPromiseRejections();

	if (result.IsEmpty()) {
		if (isolate->IsExecutionTerminating()) {
			isolate->CancelTerminateExecution();
#ifdef EXECUTION_TIMEOUT
			if (timeout) {
				throw js_error("execution timeout exceeded");
			}
#endif
			throw js_error("Out of memory error");
		}
		throw js_error(try_catch);
	}

	if (status < 0)
		throw js_error(FormatSPIStatus(status));

	return result.ToLocalChecked();
	} catch (int err) {
		elog(NOTICE, "Error caught");
	}

	return Local<v8::Value>::New(isolate, Null(isolate));
}

static Datum
CallFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	Local<Context>		context = xenv->localContext();
	Context::Scope		context_scope(context);
	Handle<v8::Value>	args[FUNC_MAX_ARGS];
	Oid fn_oid = fcinfo->flinfo->fn_oid;

 HeapTuple proctuple =
      SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);

  Oid retoid;
  Form_pg_proc pg_proc_entry = (Form_pg_proc)GETSTRUCT(proctuple);

  if (fcinfo && IsPolymorphicType(pg_proc_entry->prorettype)) {
    retoid = get_fn_expr_rettype(fcinfo->flinfo);
  } else {
    retoid = pg_proc_entry->prorettype;
  }
  ReleaseSysCache(proctuple);

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	WindowFunctionSupport support(context, fcinfo);
	Datum				ret = (Datum) 0;
	std::exception_ptr	cxx_error;
	bool				interrupt_was_active = plv8_InterruptActive();

	auto call = [&]() -> Datum {
	/* conversions run script code (getters) too */
	Plv8InterruptScope	interrupt_scope(current_context);

	/*
	 * In window function case, we cannot see the argument datum
	 * in fcinfo.  Instead, get them by WinGetFuncArgCurrent().
	 */
	if (support.IsWindowCall())
	{
		WindowObject winobj = support.GetWindowObject();
		for (int i = 0; i < nargs; i++)
		{
			bool isnull = false;
			Datum arg = (Datum) 0;

			/*
			 * Evaluating the argument can raise an error.  Turn it into a C++
			 * exception so that the destructor of support restores fcinfo in
			 * the plv8 object.
			 */
			PG_TRY();
			{
				arg = WinGetFuncArgCurrent(winobj, i, &isnull);
			}
			PG_CATCH();
			{
				throw pg_error();
			}
			PG_END_TRY();
			args[i] = ToValue(arg, isnull, &argtypes[i]);
		}
	}
	else
	{
		for (int i = 0; i < nargs; i++) {
			args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
		}
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));
	
	Local<v8::Value> result =
		DoCall(context, fn, recv, nargs, args, nonatomic);

	if (retoid == RECORDOID)
  {
    Oid calltype;
    TupleDesc tupdesc;
    get_call_result_type(fcinfo, &calltype, &tupdesc);
    if (tupdesc == NULL) {
      return ToDatum(result, &fcinfo->isnull, rettype);
    }
    plv8_type type;
    plv8_fill_type(&type, calltype);
    return ToRecordDatum(result, &fcinfo->isnull, &type, tupdesc);
  }
  else
  {
    if (rettype)
      return ToDatum(result, &fcinfo->isnull, rettype);
    else
      PG_RETURN_VOID( );
  }
	};

	/*
	 * A PostgreSQL error raised during the call jumps over the destructor of
	 * support.  Put the previous fcinfo back before passing the error on.
	 * C++ exceptions leave the PG_TRY block only after PG_END_TRY.
	 */
	PG_TRY();
	{
		try
		{
			ret = call();
		}
		catch (...)
		{
			cxx_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		support.Restore();
		/* the error skipped the destructor of interrupt_scope */
		if (!interrupt_was_active)
			plv8_InterruptDisarm();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (cxx_error)
		std::rethrow_exception(cxx_error);
	return ret;
}

static Tuplestorestate *
CreateTupleStore(PG_FUNCTION_ARGS, TupleDesc *tupdesc)
{
	Tuplestorestate	   *tupstore;

	PG_TRY();
	{
		ReturnSetInfo  *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
		MemoryContext	per_query_ctx;
		MemoryContext	oldcontext;
		plv8_proc	   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;

		/* check to see if caller supports us returning a tuplestore */
		if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("set-valued function called in context that cannot accept a set")));
		if (!(rsinfo->allowedModes & SFRM_Materialize))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("materialize mode required, but it is not " \
							"allowed in this context")));

		if (!proc->functypclass)
			proc->functypclass = get_call_result_type(fcinfo, NULL, NULL);

		per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
		oldcontext = MemoryContextSwitchTo(per_query_ctx);

		tupstore = tuplestore_begin_heap(true, false, work_mem);
		rsinfo->returnMode = SFRM_Materialize;
		rsinfo->setResult = tupstore;
		/* Build a tuple descriptor for our result type */
		if (proc->rettype.typid == RECORDOID)
		{
			if (proc->functypclass != TYPEFUNC_COMPOSITE)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("function returning record called in context "
								"that cannot accept type record")));
		}
		if (!rsinfo->setDesc)
		{
			*tupdesc = CreateTupleDescCopy(rsinfo->expectedDesc);
			rsinfo->setDesc = *tupdesc;
		}
		else
			*tupdesc = rsinfo->setDesc;

		MemoryContextSwitchTo(oldcontext);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	return tupstore;
}

static Datum
CallSRFunction(PG_FUNCTION_ARGS, plv8_exec_env *xenv,
	int nargs, plv8_type argtypes[], plv8_type *rettype)
{
	plv8_proc		   *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	TupleDesc			tupdesc;
	Tuplestorestate	   *tupstore;

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	tupstore = CreateTupleStore(fcinfo, &tupdesc);

	Handle<Context>		context = xenv->localContext();
	Context::Scope		context_scope(context);
	Converter			conv(tupdesc, proc->functypclass == TYPEFUNC_SCALAR);
	Handle<v8::Value>	args[FUNC_MAX_ARGS + 1];

	/*
	 * In case this is nested via SPI, stash pre-registered converters
	 * for the previous SRF.
	 */
	SRFSupport support(context, &conv, tupstore);
	std::exception_ptr	cxx_error;
	bool				interrupt_was_active = plv8_InterruptActive();

	auto call = [&]() {
	/* conversions run script code (getters) too */
	Plv8InterruptScope	interrupt_scope(current_context);

	for (int i = 0; i < nargs; i++) {
		args[i] = ToValue(fcinfo->args[i].value, fcinfo->args[i].isnull, &argtypes[i]);
	}

	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));

	Handle<v8::Value> result = DoCall(context, fn, recv, nargs, args, nonatomic);

	if (result->IsUndefined())
	{
		// no additional values
	}
	else if (result->IsArray())
	{
		Handle<Array> array = Handle<Array>::Cast(result);
		// return an array of records.
		int	length = array->Length();
		/* An element getter can throw. */
		TryCatch	try_catch(xenv->isolate);
		for (int i = 0; i < length; i++)
			conv.ToDatum(plv8_ToLocal(array->Get(context, i), try_catch), tupstore);
	}
	else
	{
		// return a record or a scalar
		conv.ToDatum(result, tupstore);
	}
	};

	/*
	 * A PostgreSQL error raised during the call jumps over the destructor of
	 * support.  Put the previous converter back before passing the error on.
	 * C++ exceptions leave the PG_TRY block only after PG_END_TRY.
	 */
	PG_TRY();
	{
		try
		{
			call();
		}
		catch (...)
		{
			cxx_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		support.Restore();
		/* the error skipped the destructor of interrupt_scope */
		if (!interrupt_was_active)
			plv8_InterruptDisarm();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (cxx_error)
		std::rethrow_exception(cxx_error);
	return (Datum) 0;
}

static Datum
CallTriggerBody(PG_FUNCTION_ARGS, plv8_exec_env *xenv)
{
	// trigger arguments are:
	//	0: NEW
	//	1: OLD
	//	2: TG_NAME
	//	3: TG_WHEN
	//	4: TG_LEVEL
	//	5: TG_OP
	//	6: TG_RELID
	//	7: TG_TABLE_NAME
	//	8: TG_TABLE_SCHEMA
	//	9: TG_ARGV
	TriggerData		   *trig = (TriggerData *) fcinfo->context;
	Relation			rel = trig->tg_relation;
	TriggerEvent		event = trig->tg_event;
	Handle<v8::Value>	args[10];
	Datum				result = (Datum) 0;

	bool nonatomic = fcinfo->context &&
		IsA(fcinfo->context, CallContext) &&
		!castNode(CallContext, fcinfo->context)->atomic;

	Handle<Context>		context = xenv->localContext();
	Context::Scope		context_scope(context);
	/* converting the returned row runs script code (getters) too */
	Plv8InterruptScope	interrupt_scope(current_context);

	if (TRIGGER_FIRED_FOR_ROW(event))
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);

		if (TRIGGER_FIRED_BY_INSERT(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_trigtuple);
			// OLD
			args[1] = Undefined(xenv->isolate);
		}
		else if (TRIGGER_FIRED_BY_DELETE(event))
		{
			result = PointerGetDatum(trig->tg_trigtuple);
			// NEW
			args[0] = Undefined(xenv->isolate);
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
		else if (TRIGGER_FIRED_BY_UPDATE(event))
		{
			result = PointerGetDatum(trig->tg_newtuple);
			// NEW
			args[0] = conv.ToValue(trig->tg_newtuple);
			// OLD
			args[1] = conv.ToValue(trig->tg_trigtuple);
		}
	}
	else
	{
		args[0] = args[1] = Undefined(xenv->isolate);
	}

	// 2: TG_NAME
	args[2] = ToString(trig->tg_trigger->tgname);

	// 3: TG_WHEN
	if (TRIGGER_FIRED_BEFORE(event))
		args[3] = v8::String::NewFromUtf8Literal(xenv->isolate, "BEFORE");
	else
		args[3] = v8::String::NewFromUtf8Literal(xenv->isolate, "AFTER");

	// 4: TG_LEVEL
	if (TRIGGER_FIRED_FOR_ROW(event))
		args[4] = v8::String::NewFromUtf8Literal(xenv->isolate, "ROW");
	else
		args[4] = v8::String::NewFromUtf8Literal(xenv->isolate, "STATEMENT");

	// 5: TG_OP
	if (TRIGGER_FIRED_BY_INSERT(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "INSERT");
	else if (TRIGGER_FIRED_BY_DELETE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "DELETE");
	else if (TRIGGER_FIRED_BY_UPDATE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "UPDATE");
#ifdef TRIGGER_FIRED_BY_TRUNCATE
	else if (TRIGGER_FIRED_BY_TRUNCATE(event))
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "TRUNCATE");
#endif
	else
		args[5] = v8::String::NewFromUtf8Literal(xenv->isolate, "?");

	// 6: TG_RELID
	args[6] = Uint32::New(xenv->isolate, RelationGetRelid(rel));

	// 7: TG_TABLE_NAME
	args[7] = ToString(RelationGetRelationName(rel));

	// 8: TG_TABLE_SCHEMA
	args[8] = ToString(get_namespace_name(RelationGetNamespace(rel)));

	// 9: TG_ARGV
	Handle<Array> tgargs = Array::New(xenv->isolate, trig->tg_trigger->tgnargs);
	for (int i = 0; i < trig->tg_trigger->tgnargs; i++)
		tgargs->CreateDataProperty(context, i, ToString(trig->tg_trigger->tgargs[i])).Check();
	args[9] = tgargs;

	TryCatch			try_catch(xenv->isolate);
	Local<Object> recv = Local<Object>::New(xenv->isolate, xenv->recv);
	Local<Function>		fn =
		Local<Function>::Cast(recv->GetInternalField(0));
	Handle<v8::Value> newtup =
		DoCall(context, fn, recv, lengthof(args), args, nonatomic);

	if (newtup.IsEmpty())
		throw js_error(try_catch);

	/*
	 * If the function specifically returned null, return NULL to
	 * tell executor to skip the operation.  Otherwise, the function
	 * result is the tuple to be returned.
	 */
	if (newtup->IsNull() || !TRIGGER_FIRED_FOR_ROW(event))
	{
		result = PointerGetDatum(NULL);
	}
	else if (!newtup->IsUndefined())
	{
		TupleDesc		tupdesc = RelationGetDescr(rel);
		Converter		conv(tupdesc);
		HeapTupleHeader	header;

		header = DatumGetHeapTupleHeader(conv.ToDatum(newtup));

		/* We know it's there; heap_form_tuple stores with this layout. */
		result = PointerGetDatum((char *) header - HEAPTUPLESIZE);
	}

	return result;
}

static Datum
CallTrigger(PG_FUNCTION_ARGS, plv8_exec_env *xenv)
{
	bool				interrupt_was_active = plv8_InterruptActive();
	Datum				ret = (Datum) 0;
	std::exception_ptr	cxx_error;

	/* As in CallFunction(), a PostgreSQL error skips the scope destructors. */
	PG_TRY();
	{
		try
		{
			ret = CallTriggerBody(fcinfo, xenv);
		}
		catch (...)
		{
			cxx_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		if (!interrupt_was_active)
			plv8_InterruptDisarm();
		PG_RE_THROW();
	}
	PG_END_TRY();

	if (cxx_error)
		std::rethrow_exception(cxx_error);
	return ret;
}

Datum
plv8_call_validator(PG_FUNCTION_ARGS)
{
	plv8_context   *context = GetPlv8ContextOrError();
	Oid				fn_oid = PG_GETARG_OID(0);
	HeapTuple		tuple;
	Form_pg_proc	proc;
	char			functyptype;
	bool			is_trigger = false;
	CurrentContextScope	context_scope(context);
	Isolate::Scope  scope(context->isolate);

	if (!CheckFunctionValidatorAccess(fcinfo->flinfo->fn_oid, fn_oid))
		PG_RETURN_VOID();

	/* Get the new function's pg_proc entry */
	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);

	functyptype = get_typtype(proc->prorettype);

	/* Disallow pseudotype result */
	/* except for TRIGGER, RECORD, INTERNAL, VOID or polymorphic types */
	if (functyptype == TYPTYPE_PSEUDO)
	{
    if (proc->prorettype == TRIGGEROID)
			is_trigger = true;
		else if (proc->prorettype != RECORDOID &&
			proc->prorettype != VOIDOID &&
			proc->prorettype != INTERNALOID &&
			!IsPolymorphicType(proc->prorettype))
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("PL/v8 functions cannot return type %s",
						format_type_be(proc->prorettype))));
	}

	ReleaseSysCache(tuple);

	try
	{
		/* Don't use validator's fcinfo */
		plv8_proc	   *proc = Compile(fn_oid, NULL,
									   true, is_trigger);
		(void) CreateExecEnv(proc->cache->function, context);
		/* the result of a validator is ignored */
		PG_RETURN_VOID();
	}
	catch (js_error& e)	{ e.rethrow(); }
	catch (pg_error& e)	{ e.rethrow(); }

	return (Datum) 0;	// keep compiler quiet
}

static plv8_proc *
plv8_get_proc(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, char ***argnames) throw()
{
	HeapTuple			procTup;
	plv8_proc_cache	   *cache;
	bool				found;
	bool				isnull;
	Datum				prosrc;
	Oid				   *argtypes;
	char			   *argmodes;
	MemoryContext		oldcontext;

	procTup = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(procTup))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);

	cache = (plv8_proc_cache *)
		hash_search(plv8_proc_cache_hash,&fn_oid, HASH_ENTER, &found);

	if (found)
	{
		bool	uptodate;

		/*
		 * We need to check user id and dispose it if it's different from
		 * the previous cache user id, as the V8 function is associated
		 * with the context where it was generated.  In most cases,
		 * we can expect this doesn't affect runtime performance.
		 */
		uptodate = (!cache->function.IsEmpty() &&
			cache->fn_xmin == plv8_heap_tuple_get_xmin(procTup) &&
			ItemPointerEquals(&cache->fn_tid, &procTup->t_self) &&
			cache->user_id == current_context->user_id);

		if (!uptodate)
		{
			if (cache->prosrc)
			{
				pfree(cache->prosrc);
				cache->prosrc = NULL;
			}
			cache->function.Reset();
		}
		else
		{
			ReleaseSysCache(procTup);
		}
	}
	else
	{
		new(&cache->function) Persistent<Function>();
		cache->prosrc = NULL;
	}

	if (cache->function.IsEmpty())
	{
		Form_pg_proc	procStruct;

		procStruct = (Form_pg_proc) GETSTRUCT(procTup);

		prosrc = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isnull);
		if (isnull)
			elog(ERROR, "null prosrc");

		cache->retset = procStruct->proretset;
		cache->rettype = procStruct->prorettype;

		strlcpy(cache->proname, NameStr(procStruct->proname), NAMEDATALEN);
		cache->fn_xmin = plv8_heap_tuple_get_xmin(procTup);
		cache->fn_tid = procTup->t_self;
		cache->user_id = current_context->user_id;

		int nargs = get_func_arg_info(procTup, &argtypes, argnames, &argmodes);

		if (validate)
		{
			/*
			 * Disallow non-polymorphic pseudotypes in arguments
			 * (either IN or OUT).  Internal type is used to declare
			 * js functions for find_function().
			 */
			for (int i = 0; i < nargs; i++)
			{
				if (get_typtype(argtypes[i]) == TYPTYPE_PSEUDO &&
						argtypes[i] != INTERNALOID &&
						!IsPolymorphicType(argtypes[i]))
					ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("PL/v8 functions cannot accept type %s",
								format_type_be(argtypes[i]))));
			}
		}

		oldcontext = MemoryContextSwitchTo(TopMemoryContext);
		cache->prosrc = TextDatumGetCString(prosrc);
		MemoryContextSwitchTo(oldcontext);

		ReleaseSysCache(procTup);

		int	inargs = 0;
		for (int i = 0; i < nargs; i++)
		{
			Oid		argtype = argtypes[i];
			char	argmode = argmodes ? argmodes[i] : PROARGMODE_IN;

			switch (argmode)
			{
			case PROARGMODE_IN:
			case PROARGMODE_INOUT:
			case PROARGMODE_VARIADIC:
				break;
			default:
				continue;
			}

			if (*argnames)
				(*argnames)[inargs] = (*argnames)[i];
			cache->argtypes[inargs] = argtype;
			inargs++;
		}
		cache->nargs = inargs;
	}

	MemoryContext mcxt = CurrentMemoryContext;
	if (fcinfo)
		mcxt = fcinfo->flinfo->fn_mcxt;

	plv8_proc *proc = (plv8_proc *) MemoryContextAllocZero(mcxt,
		offsetof(plv8_proc, argtypes) + sizeof(plv8_type) * cache->nargs);

	proc->cache = cache;
	for (int i = 0; i < cache->nargs; i++)
	{
		Oid		argtype = cache->argtypes[i];
		/* Resolve polymorphic types, if this is an actual call context. */
		if (fcinfo && IsPolymorphicType(argtype))
			argtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		plv8_fill_type(&proc->argtypes[i], argtype, mcxt);
	}

	Oid		rettype = cache->rettype;
	/* Resolve polymorphic return type if this is an actual call context. */
	if (fcinfo && IsPolymorphicType(rettype))
		rettype = get_fn_expr_rettype(fcinfo->flinfo);
	plv8_fill_type(&proc->rettype, rettype, mcxt);

	return proc;
}

static plv8_exec_env *
CreateExecEnv(Persistent<Function>& function, plv8_context *context)
{
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(context->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(context);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	xenv->context.Reset(context->isolate, context->context);
	Local<Context>		ctx = xenv->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(context->isolate, context->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(context->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(context->isolate, obj);


	return xenv;
}

static plv8_exec_env *
CreateExecEnv(Handle<Function> function, plv8_context *context)
{
	Isolate::Scope	    iscope(context->isolate);
	plv8_exec_env	   *xenv;
	HandleScope			handle_scope(context->isolate);

	PG_TRY();
	{
		xenv = plv8_new_exec_env(context);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	xenv->context.Reset(context->isolate, context->context);
	Local<Context>		ctx = xenv->localContext();
	Context::Scope		scope(ctx);

	Local<ObjectTemplate> templ = Local<ObjectTemplate>::New(context->isolate, context->recv_templ);
	Local<Object> obj = templ->NewInstance(ctx).ToLocalChecked();
	Local<Function> f = Local<Function>::New(context->isolate, function);
	obj->SetInternalField(0, f);
	xenv->recv.Reset(context->isolate, obj);


	return xenv;
}

/*
 * fcinfo should be passed if this is an actual function call context, where
 * we can resolve polymorphic types and use function's memory context.
 */
static plv8_proc *
Compile(Oid fn_oid, FunctionCallInfo fcinfo, bool validate, bool is_trigger)
{
	plv8_proc  *proc;
	char	  **argnames;

	PG_TRY();
	{
		proc = plv8_get_proc(fn_oid, fcinfo, validate, &argnames);
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();

	plv8_proc_cache *cache = proc->cache;

	if (cache->function.IsEmpty())
	{
		/*
		 * Compile into current_context, which the caller has already set
		 * up (and whose startup procedure, if any, has already run): the
		 * call handler and the validator point it at the effective user's
		 * context, and find_js_function() is invoked while that context
		 * is executing.  Looking the context up again here by user id
		 * would compile into the wrong isolate when they differ, and
		 * could re-enter Compile() through the startup procedure while
		 * this cache entry is still being filled in.
		 */
		Isolate::Scope	scope(current_context->isolate);
		HandleScope		handle_scope(current_context->isolate);
		cache->function.Reset(current_context->isolate, CompileFunction(
						current_context,
						cache->proname,
						cache->nargs,
						(const char **) argnames,
						cache->prosrc,
						is_trigger,
						cache->retset));
	}

	return proc;
}

static Local<Function>
CompileFunction(
	plv8_context *global_context,
	const char *proname,
	int proarglen,
	const char *proargs[],
	const char *prosrc,
	bool is_trigger,
	bool retset)
{
	Isolate					   *isolate = Isolate::GetCurrent();
	EscapableHandleScope		handle_scope(isolate);
	StringInfoData	src;

	initStringInfo(&src);

	appendStringInfo(&src, "(function (");
	if (is_trigger)
	{
		if (proarglen != 0)
			throw js_error("trigger function cannot have arguments");
		// trigger function has special arguments.
		appendStringInfo(&src,
			"NEW, OLD, TG_NAME, TG_WHEN, TG_LEVEL, TG_OP, "
			"TG_RELID, TG_TABLE_NAME, TG_TABLE_SCHEMA, TG_ARGV");
	}
	else
	{
		for (int i = 0; i < proarglen; i++)
		{
			if (i > 0)
				appendStringInfoChar(&src, ',');
			if (proargs && proargs[i])
				appendStringInfoString(&src, proargs[i]);
			else
				appendStringInfo(&src, "$%d", i + 1);	// unnamed argument to $N
		}
	}

	appendStringInfo(&src, "){\n%s\n})", prosrc);

	Handle<v8::Value> name;
	if (proname)
		name = ToString(proname);
	else
		name = Undefined(isolate);
	Local<v8::String> source = ToString(src.data, src.len);
	pfree(src.data);

	Local<Context> context = Local<Context>::New(isolate, global_context->context);
	Context::Scope	context_scope(context);
	TryCatch		try_catch(isolate);
	v8::ScriptOrigin origin(isolate, name);

#ifdef EXECUTION_TIMEOUT
	bool timeout = false;
#ifdef _MSC_VER // windows
	HANDLE  hThread;
	DWORD dwThreadId;
	hThread = CreateThread(NULL, 0, Breakout, isolate, 0, &dwThreadId);
#else
	pthread_t breakout_thread;
	// set up the thread to break out the execution if needed
	pthread_create(&breakout_thread, NULL, Breakout, isolate);
#endif
#endif

	v8::Local<v8::Script> script;
	v8::Local<v8::Value> result;
	if (current_context->interrupted) {
		isolate->CancelTerminateExecution();
		current_context->interrupted = false;
	}
	{
		/* the source can break out of the function wrapper and run code */
		Plv8InterruptScope	interrupt_scope(global_context);

		if (Script::Compile(isolate->GetCurrentContext(), source, &origin).ToLocal(&script)) {
			if (!script.IsEmpty()) {
				if (!script->Run(isolate->GetCurrentContext()).ToLocal(&result))
					plv8_ThrowScriptError(try_catch);
			}
		}
	}

#ifdef EXECUTION_TIMEOUT
#ifdef _MSC_VER
	if (TerminateThread(hThread, NULL) == 0) {
		timeout = true;
	}
#else
	void *thread_result;
	pthread_cancel(breakout_thread);
	pthread_join(breakout_thread, &thread_result);
	if (thread_result == NULL) {
		timeout = true;
	}
#endif
#endif

	HandleUnhandledPromiseRejections();

	if (result.IsEmpty()) {
		if (isolate->IsExecutionTerminating()  || current_context->interrupted) {
			isolate->CancelTerminateExecution();
			if (current_context->interrupted) {
				current_context->interrupted = false;
				plv8_ReportInterrupt();
			}
#ifdef EXECUTION_TIMEOUT
			if (timeout) {
				throw js_error("compiler timeout exceeded");
			}
#endif
			throw js_error("Script is out of memory");
		}
		throw js_error(try_catch);
	}

	return handle_scope.Escape(Local<Function>::Cast(result));
}

Local<Function>
find_js_function(Oid fn_oid)
{
	HeapTuple		tuple;
	Form_pg_proc	proc;
	Oid				prolang;
	NameData		langnames[] = { {"plv8"} };
	int				langno;
	int				langlen = sizeof(langnames) / sizeof(NameData);
	Local<Function> func;
	Isolate			*isolate = Isolate::GetCurrent();

	tuple = SearchSysCache(PROCOID, ObjectIdGetDatum(fn_oid), 0, 0, 0);
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for function %u", fn_oid);
	proc = (Form_pg_proc) GETSTRUCT(tuple);
	prolang = proc->prolang;
	ReleaseSysCache(tuple);

	/* Should not happen? */
	if (!OidIsValid(prolang))
		return func;

	/* See if the function language is a compatible one */
	for (langno = 0; langno < langlen; langno++)
	{
		tuple = SearchSysCache(LANGNAME, NameGetDatum(&langnames[langno]), 0, 0, 0);
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_database datForm = (Form_pg_database) GETSTRUCT(tuple);
			Oid langtupoid = datForm->oid;

			ReleaseSysCache(tuple);
			if (langtupoid == prolang)
				break;
		}
	}

	/* Not found or non-JS function */
	if (langno >= langlen)
		return func;

	try
	{
		plv8_proc		   *proc = Compile(fn_oid, NULL,
										   true, false);

		TryCatch			try_catch(isolate);

		func = Local<Function>::New(isolate, proc->cache->function);
	}
	catch (js_error& e) { e.rethrow(); }
	catch (pg_error& e) { e.rethrow(); }

	return func;
}

/*
 * NOTICE: the returned buffer could be an internal static buffer.
 */
const char *
FormatSPIStatus(int status) throw()
{
	static char	private_buf[1024];

	if (status > 0)
		return "OK";

	switch (status)
	{
		case SPI_ERROR_CONNECT:
			return "SPI_ERROR_CONNECT";
		case SPI_ERROR_COPY:
			return "SPI_ERROR_COPY";
		case SPI_ERROR_OPUNKNOWN:
			return "SPI_ERROR_OPUNKNOWN";
		case SPI_ERROR_UNCONNECTED:
		case SPI_ERROR_TRANSACTION:
			return "current transaction is aborted, "
				   "commands ignored until end of transaction block";
		case SPI_ERROR_CURSOR:
			return "SPI_ERROR_CURSOR";
		case SPI_ERROR_ARGUMENT:
			return "SPI_ERROR_ARGUMENT";
		case SPI_ERROR_PARAM:
			return "SPI_ERROR_PARAM";
		case SPI_ERROR_NOATTRIBUTE:
			return "SPI_ERROR_NOATTRIBUTE";
		case SPI_ERROR_NOOUTFUNC:
			return "SPI_ERROR_NOOUTFUNC";
		case SPI_ERROR_TYPUNKNOWN:
			return "SPI_ERROR_TYPUNKNOWN";
		default:
			snprintf(private_buf, sizeof(private_buf),
				"SPI_ERROR: %d", status);
			return private_buf;
	}
}

static text *
charToText(char *string)
{
	int len = strlen(string);
	text *result = (text *) palloc(len + 1 + VARHDRSZ);

	SET_VARSIZE(result, len + VARHDRSZ);
	memcpy(VARDATA(result), string, len + 1);

	return result;
}

static plv8_context*
GetPlv8Context() {
	Oid					user_id = GetUserId();
	unsigned int		i;
	plv8_context		*my_context = nullptr;

	for (i = 0; i < ContextVector.size(); i++)
	{
		if (ContextVector[i]->user_id == user_id)
		{
			my_context = ContextVector[i];
			break;
		}
	}
	if (my_context && (my_context->is_dead || (my_context->isolate && my_context->isolate->IsDead()))) {
		// the isolate is dead because of OOM, kill it and dispose
		char *username = GetUserNameFromId(my_context->user_id, false);
		elog(LOG_SERVER_ONLY, "Disposing of a dead isolate for: %s", username);
		ContextVector.erase(ContextVector.begin() + i);
		if (my_context->isolate && my_context->isolate->IsInUse()) {
			my_context->isolate->Exit();
		}
		killPlv8Context(my_context);
		pfree(my_context);
		my_context = nullptr;
	}
	if (!my_context)
	{
		my_context = (plv8_context *) MemoryContextAllocZero(TopMemoryContext,
														 sizeof(plv8_context));
		my_context->is_dead = false;
		my_context->interrupted = false;
		my_context->ignore_unhandled_promises = false;
		CreateIsolate(my_context);
		Isolate 			   *isolate = my_context->isolate;
		Isolate::Scope			scope(isolate);
		HandleScope				handle_scope(isolate);

		Local<ObjectTemplate>	global = Local<ObjectTemplate>::New(isolate, GetGlobalObjectTemplate(isolate));

		new(&my_context->context) Persistent<Context>();
		my_context->context.Reset(isolate, Context::New(isolate, NULL, global));
		my_context->user_id = user_id;
		my_context->id = next_context_id++;

		new(&my_context->recv_templ) Persistent<ObjectTemplate>();
		Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
		templ->SetInternalFieldCount(1);
		my_context->recv_templ.Reset(isolate, templ);

		new(&my_context->compile_context) Persistent<Context>();
		Local<Context> ctx = Context::New(isolate, (ExtensionConfiguration*)NULL);
		my_context->compile_context.Reset(isolate, ctx);

		auto toStringAttr = static_cast<PropertyAttribute>(v8::ReadOnly | v8::DontEnum);
		auto toStringSymbol = v8::Symbol::GetToStringTag(isolate);

		new(&my_context->plan_template) Persistent<ObjectTemplate>();
		Local<FunctionTemplate> base = FunctionTemplate::New(isolate);
		Local<v8::String> planClassName = v8::String::NewFromUtf8Literal(isolate, "PreparedPlan",
														   NewStringType::kInternalized);
		base->SetClassName(planClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, planClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupPrepFunctions(templ);
		my_context->plan_template.Reset(isolate, templ);

		new(&my_context->cursor_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> cursorClassName = v8::String::NewFromUtf8Literal(isolate, "Cursor",
															 NewStringType::kInternalized);
		base->SetClassName(cursorClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, cursorClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupCursorFunctions(templ);
		my_context->cursor_template.Reset(isolate, templ);

		new(&my_context->window_template) Persistent<ObjectTemplate>();
		base = FunctionTemplate::New(isolate);
		Local<v8::String> windowClassName = v8::String::NewFromUtf8Literal(isolate, "WindowObject",
															 NewStringType::kInternalized);
        base->SetClassName(windowClassName);
		base->PrototypeTemplate()->Set(toStringSymbol, windowClassName, toStringAttr);
		templ = base->InstanceTemplate();
		SetupWindowFunctions(templ);
		my_context->window_template.Reset(isolate, templ);
		/*
		 * Need to register it before running any code, as the code
		 * recursively may want to the global context.
		 */
		ContextVector.push_back(my_context);

		/*
		 * Run the start up procedure if configured.
		 */
		if (plv8_start_proc != NULL)
		{
			Local<Function>		func;

			CurrentContextScope	current_scope(my_context);
			HandleScope			handle_scope(isolate);
			Local<Context>		context = my_context->localContext();
			Context::Scope		context_scope(context);
			TryCatch			try_catch(isolate);
			MemoryContext		ctx = CurrentMemoryContext;
			text *arg;
			// Stack-allocate FunctionCallInfoBaseData with
			// space for 2 arguments:
			LOCAL_FCINFO(fake_fcinfo, 2);
			FmgrInfo	flinfo;

			char perm[16];
			strcpy(perm, "EXECUTE");
			arg = charToText(perm);

			PG_TRY();
					{
						Oid funcoid = DatumGetObjectId(DirectFunctionCall1(regprocin, CStringGetDatum(plv8_start_proc)));
						MemSet(&flinfo, 0, sizeof(flinfo));
						fake_fcinfo->flinfo = &flinfo;
						flinfo.fn_oid = InvalidOid;
						flinfo.fn_mcxt = CurrentMemoryContext;
						fake_fcinfo->nargs = 2;
						fake_fcinfo->args[0].value = ObjectIdGetDatum(funcoid);
						fake_fcinfo->args[1].value = PointerGetDatum(arg);
						Datum ret = has_function_privilege_id(fake_fcinfo);

						if (ret == 0) {
							elog(WARNING, "failed to find js function %s", plv8_start_proc);
						} else {
							if (DatumGetBool(ret)) {
								func = find_js_function(funcoid);
							} else {
								elog(WARNING, "no permission to execute js function %s", plv8_start_proc);
							}
						}
					}
				PG_CATCH();
					{
						ErrorData	   *edata;

						MemoryContextSwitchTo(ctx);
						edata = CopyErrorData();
						elog(WARNING, "failed to find js function %s", edata->message);
						FlushErrorState();
						FreeErrorData(edata);
					}
			PG_END_TRY();

			pfree(arg);

			if (!func.IsEmpty())
			{
				Handle<v8::Value>	result =
						DoCall(context, func, my_context->localContext()->Global(), 0, NULL, false);
				if (result.IsEmpty())
					throw js_error(try_catch);
			}
		}
	}
	return my_context;
}

static Local<ObjectTemplate>
GetGlobalObjectTemplate(Isolate *isolate)
{
	Persistent<ObjectTemplate>	global;

	if (global.IsEmpty())
	{
		HandleScope				handle_scope(isolate);

		Local<ObjectTemplate> templ = ObjectTemplate::New(isolate);
		// ERROR levels for elog
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG5", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG5));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG4", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG4));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG3", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG3));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG2", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG2));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG1", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG1));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "DEBUG", NewStringType::kInternalized),
				   Int32::New(isolate, DEBUG5));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "LOG", NewStringType::kInternalized),
				   Int32::New(isolate, LOG));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "INFO", NewStringType::kInternalized),
				   Int32::New(isolate, INFO));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "NOTICE", NewStringType::kInternalized),
				   Int32::New(isolate, NOTICE));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "WARNING", NewStringType::kInternalized),
				   Int32::New(isolate, WARNING));
		templ->Set(v8::String::NewFromUtf8Literal(isolate, "ERROR", NewStringType::kInternalized),
				   Int32::New(isolate, ERROR));
		global.Reset(isolate, templ);

		Local<ObjectTemplate> plv8 = ObjectTemplate::New(isolate);

		SetupPlv8Functions(plv8);
		plv8->Set(v8::String::NewFromUtf8Literal(isolate, "version", NewStringType::kInternalized),
				  v8::String::NewFromUtf8Literal(isolate, PLV8_VERSION));
		plv8->Set(v8::String::NewFromUtf8Literal(isolate, "v8_version", NewStringType::kInternalized),
				  v8::String::NewFromUtf8Literal(isolate, V8_VERSION_STRING));

		templ->Set(v8::String::NewFromUtf8Literal(isolate, "plv8", NewStringType::kInternalized), plv8);
	}
	return Local<ObjectTemplate>::New(isolate, global);
}

/*
 * Accessor to plv8_type stored in fcinfo.
 */
plv8_type *
get_plv8_type(PG_FUNCTION_ARGS, int argno)
{
	plv8_proc *proc = (plv8_proc *) fcinfo->flinfo->fn_extra;
	return &proc->argtypes[argno];
}

Converter::Converter(TupleDesc tupdesc) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(false),
	m_memcontext(NULL)
{
	Init();
}

Converter::Converter(TupleDesc tupdesc, bool is_scalar) :
	m_tupdesc(tupdesc),
	m_colnames(tupdesc->natts),
	m_coltypes(tupdesc->natts),
	m_is_scalar(is_scalar),
	m_memcontext(NULL)
{
	Init();
}

Converter::~Converter()
{
	if (m_memcontext != NULL)
	{
		MemoryContext ctx = CurrentMemoryContext;

		PG_TRY();
		{
			MemoryContextDelete(m_memcontext);
		}
		PG_CATCH();
		{
			ErrorData	   *edata;

			MemoryContextSwitchTo(ctx);
			// don't throw out from deconstructor
			edata = CopyErrorData();
			elog(WARNING, "~Converter: %s", edata->message);
			FlushErrorState();
			FreeErrorData(edata);
		}
		PG_END_TRY();
		m_memcontext = NULL;
	}
}

void
Converter::Init()
{
	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			continue;

		m_colnames[c] = ToString(NameStr(TupleDescAttr(m_tupdesc, c)->attname));

		PG_TRY();
		{
			if (m_memcontext == NULL)
				m_memcontext = AllocSetContextCreate(
									CurrentMemoryContext,
									"ConverterContext",
									ALLOCSET_DEFAULT_SIZES);
			plv8_fill_type(&m_coltypes[c],
						   TupleDescAttr(m_tupdesc, c)->atttypid,
						   m_memcontext);
		}
		PG_CATCH();
		{
			throw pg_error();
		}
		PG_END_TRY();
	}
}

// TODO: use prototype instead of per tuple fields to reduce
// memory consumption.
Local<Object>
Converter::ToValue(HeapTuple tuple)
{
	Isolate		   *isolate = Isolate::GetCurrent();
    Local<Context>  context = isolate->GetCurrentContext();
	Local<Object>	obj = Object::New(isolate);

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		Datum		datum;
		bool		isnull;

		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
			continue;

		datum = heap_getattr(tuple, c + 1, m_tupdesc, &isnull);

		obj->CreateDataProperty(context, m_colnames[c], ::ToValue(datum, isnull, &m_coltypes[c])).Check();
	}

	return obj;
}

Datum
Converter::ToDatum(Handle<v8::Value> value, Tuplestorestate *tupstore)
{
	Isolate		   *isolate = Isolate::GetCurrent();
    Local<Context>  context = isolate->GetCurrentContext();
	Datum			result;
	TryCatch		try_catch(isolate);
	Handle<Object>	obj;

	if (!m_is_scalar)
	{
		if (!value->IsObject())
			throw js_error("argument must be an object");
		obj = Handle<Object>::Cast(value);
		if (obj.IsEmpty())
			throw js_error(try_catch);
	}

	/*
	 * Use vector<char> instead of vector<bool> because <bool> version is
	 * s specialized and different from bool[].
	 */
	Datum  *values = (Datum *) palloc(sizeof(Datum) * m_tupdesc->natts);
	bool   *nulls = (bool *) palloc(sizeof(bool) * m_tupdesc->natts);

	/* The script can catch a conversion error, free the work arrays anyway. */
	try
	{
	if (!m_is_scalar)
	{
		Handle<Array> names = plv8_ToLocal(obj->GetPropertyNames(isolate->GetCurrentContext()), try_catch);

		for (int c = 0; c < m_tupdesc->natts; c++)
		{
			if (TupleDescAttr(m_tupdesc, c)->attisdropped)
				continue;

			bool found = false;
			CString  colname(m_colnames[c]);
			for (int d = 0; d < m_tupdesc->natts; d++)
			{
				/* Past the end of names, Get() reads Array.prototype. */
				CString fname(plv8_ToStringChecked(isolate, plv8_ToLocal(names->Get(context, d), try_catch)));
				if (strcmp(colname, fname) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found)
				throw js_error("field name / property name mismatch");
		}
	}

	for (int c = 0; c < m_tupdesc->natts; c++)
	{
		/* Make sure dropped columns are skipped by backend code. */
		if (TupleDescAttr(m_tupdesc, c)->attisdropped)
		{
			nulls[c] = true;
			continue;
		}

		Handle<v8::Value> attr = m_is_scalar ? value : plv8_ToLocal(obj->Get(context, m_colnames[c]), try_catch);
		if (attr.IsEmpty() || attr->IsUndefined() || attr->IsNull())
			nulls[c] = true;
		else
			values[c] = ::ToDatum(attr, &nulls[c], &m_coltypes[c]);
	}

	PG_TRY();
	{
		if (tupstore)
		{
			tuplestore_putvalues(tupstore, m_tupdesc, values, nulls);
			result = (Datum) 0;
		}
		else
		{
			result = HeapTupleGetDatum(heap_form_tuple(m_tupdesc, values, nulls));
		}
	}
	PG_CATCH();
	{
		throw pg_error();
	}
	PG_END_TRY();
	}
	catch (...)
	{
		pfree(values);
		pfree(nulls);
		throw;
	}

	pfree(values);
	pfree(nulls);

	return result;
}

/*
 * Throw js_error for a V8 call that returned an empty result.
 *
 * Termination (the memory limit, an interrupt) has no script exception, and
 * a call made while V8 is terminating returns empty without catching one.
 * These get the messages that DoCall() uses.
 */
void
plv8_ThrowScriptError(v8::TryCatch &try_catch)
{
	if (try_catch.HasTerminated() || Isolate::GetCurrent()->IsExecutionTerminating())
	{
		if (current_context != NULL && current_context->interrupted)
			plv8_ReportInterrupt();
		throw js_error("Out of memory error");
	}
	if (!try_catch.HasCaught())
		throw js_error("unknown exception");
	throw js_error(try_catch);
}

/*
 * Copy the message strings into CurrentMemoryContext, for an error made in
 * a memory context that is deleted before the error is reported.
 */
void
js_error::copy_strings()
{
	if (m_msg)
		m_msg = pstrdup(m_msg);
	if (m_detail)
		m_detail = pstrdup(m_detail);
	if (m_hint)
		m_hint = pstrdup(m_hint);
	if (m_context)
		m_context = pstrdup(m_context);
}

js_error::js_error() noexcept
	: m_msg(nullptr), m_code(0), m_detail(nullptr), m_hint(nullptr), m_context(nullptr)
{
}

js_error::js_error(const char *msg) noexcept : js_error()
{
	m_msg = pstrdup(msg);
}

js_error::js_error(Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<v8::Message> message) noexcept : js_error() {
	init(isolate, exception, message);
}

js_error::js_error(v8::TryCatch &try_catch) noexcept : js_error() {
	Isolate		   		*isolate = Isolate::GetCurrent();
	HandleScope			handle_scope(isolate);

	init(isolate, try_catch.Exception(), try_catch.Message());
}

void
js_error::init(Isolate *isolate, v8::Local<v8::Value> exception, v8::Local<Message> message) noexcept
{
	HandleScope			handle_scope(isolate);
	Local<Context>      context = isolate->GetCurrentContext();

	try
	{
		/*
		 * Converting the exception to a string may fail, e.g. when the
		 * JavaScript stack is exhausted (infinite recursion) and
		 * toString() cannot run, or when the exception is empty.  In
		 * that case Utf8Value yields NULL, so fall back to the message
		 * text V8 already formatted, and then to a generic message,
		 * rather than dereferencing NULL below.
		 */
		if (!exception.IsEmpty())
		{
			v8::String::Utf8Value	err_message(isolate, exception);
			m_msg = ToCStringCopy(err_message);
		}
		if (m_msg == NULL && !message.IsEmpty())
		{
			v8::String::Utf8Value	err_message(isolate, message->Get());
			m_msg = ToCStringCopy(err_message);
		}
		if (m_msg == NULL)
			m_msg = pstrdup("unknown exception");

        StringInfoData	detailStr;
        StringInfoData	hintStr;
        StringInfoData	contextStr;
        initStringInfo(&detailStr);
        initStringInfo(&hintStr);
        initStringInfo(&contextStr);
		Handle<v8::Object> err;
		if (exception->ToObject(context).ToLocal(&err))
        {
            if (!err.IsEmpty())
            {
                v8::Local<v8::Value> errCode;
                if (err->Get(context,
                             v8::String::NewFromUtf8Literal(isolate, "code")).ToLocal(&errCode))
                {
                    if (!errCode->IsUndefined() && !errCode->IsNull())
                    {
                        int32_t code;
                        if (errCode->Int32Value(context).To(&code))
                            m_code = code;
                    }
                }

                v8::Local<v8::Value> errDetail;
                if (err->Get(context,
                             v8::String::NewFromUtf8Literal(isolate, "detail")).ToLocal(&errDetail))
                {
                    if (!errDetail->IsUndefined() && !errDetail->IsNull())
                    {
                        CString detail(errDetail);
                        appendStringInfo(&detailStr, "%s", detail.str("?"));
                        m_detail = detailStr.data;
                    }
                }

                v8::Local<v8::Value> errHint;
                if (err->Get(context,
                             v8::String::NewFromUtf8Literal(isolate, "hint")).ToLocal(&errHint))
                {
                    if (!errHint->IsUndefined() && !errHint->IsNull())
                    {
                        CString hint(errHint);
                        appendStringInfo(&hintStr, "%s", hint.str("?"));
                        m_hint = hintStr.data;
                    }
                }

                v8::Local<v8::Value> errContext;
                if (err->Get(context, v8::String::NewFromUtf8Literal(isolate, "context")).ToLocal(&errContext))
                    if (!errContext->IsUndefined() && !errContext->IsNull())
                    {
                        CString str_context(errContext);
                        appendStringInfo(&contextStr, "%s\n", str_context.str("?"));
                    }
            }
        }

		if (!message.IsEmpty())
		{
			CString		script(message->GetScriptResourceName());
			int		lineno = message->GetLineNumber(context).FromMaybe(1);
			Local<v8::String>	sourceLine;
			/* leaves sourceLine empty on failure, printed as "?" */
			(void) message->GetSourceLine(context).ToLocal(&sourceLine);
			CString		source(sourceLine);
			// TODO: Get stack trace?
			//Handle<StackTrace> stackTrace(message->GetStackTrace());

			/*
			 * Report lineno - 1 because "function _(...){" was added
			 * at the first line to the javascript code.
			 */
			if (strstr(m_msg, "Error: ") == m_msg)
				m_msg += 7;

			appendStringInfo(&contextStr, "%s() LINE %d: %s",
				script.str("?"), lineno - 1, source.str("?"));
		}

		m_context = contextStr.data;
	}
	catch (...)
	{
		// nested error, keep quiet.
	}
}

Local<v8::Value>
js_error::error_object()
{
	char *msg = pstrdup(m_msg ? m_msg : "unknown exception");
	/*
	 * Trim leading "Error: ", in case the message is generated from
	 * another Error.
	 */
	if (strstr(msg, "Error: ") == msg)
		msg += 7;
	Local<v8::String> message = ToString(msg);
	return Exception::Error(message);
}

void
js_error::log(int elevel, const char *msg_format) noexcept {
	if (elevel >= ERROR) {
		return rethrow(msg_format);
	}
	ereport(elevel,
			(
					m_code ? errcode(m_code): 0,
					m_msg ? errmsg((msg_format ? msg_format : "%s"), m_msg) : 0,
					m_detail ? errdetail("%s", m_detail) : 0,
					m_hint ? errhint("%s", m_hint) : 0,
					m_context ? errcontext("%s", m_context) : 0
			));
}

__attribute__((noreturn))
void
js_error::rethrow(const char *msg_format) noexcept
{
	ereport(ERROR,
			(
					m_code ? errcode(m_code): 0,
					m_msg ? errmsg((msg_format ? msg_format : "%s"), m_msg) : 0,
					m_detail ? errdetail("%s", m_detail) : 0,
					m_hint ? errhint("%s", m_hint) : 0,
					m_context ? errcontext("%s", m_context) : 0
			));
	exit(0);	// keep compiler quiet
}

__attribute__((noreturn))
void
pg_error::rethrow() throw()
{
	PG_RE_THROW();
	exit(0);	// keep compiler quiet
}
