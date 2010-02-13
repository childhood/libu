#include <stddef.h>
#include <u/libu.h>
#include "test.h"

#define TEST_ID_MAX     128

#define TEST_OUTFN_DFL  "./report.txt"  /* Default test report file name */

/* Generic test objects list header. */
typedef struct 
{ 
    unsigned int currank, nchildren;
    LIST_HEAD(, test_obj_s) objs;
} TO;

/* Test dependencies list header. */
typedef LIST_HEAD(, test_dep_s) TD;

/* A test suite/case dependency (identified by its label). */
struct test_dep_s
{
    char id[TEST_ID_MAX];           /* Tag of the declared dependency */
    LIST_ENTRY(test_dep_s) links;   /* Sibling deps */
};

typedef struct test_dep_s test_dep_t;

/* Generic test data container.  The attributes shared by both test suites and
 * cases are stored here.  The parent container (be it suite or case depends 
 * on '.what' attribute value) is accessed via the T[SC]_HANDLE() macro. */
struct test_obj_s
{
    test_what_t what;               /* Test suite/case */
    TO *parent;                     /* Head of the list we're attached to */
    _Bool sequenced;                /* True if sequenced */
    unsigned int rank;              /* Scheduling rank: low have higher prio */
    char id[TEST_ID_MAX];           /* Test object identifier: MUST be unique */
    int status;                     /* Test exit code: TEST_{SUCCESS,FAILURE} */
    time_t start, stop;             /* Test case/suite begin/end timestamps */
    LIST_ENTRY(test_obj_s) links;   /* Sibling test cases/suites */
    TD deps;                        /* Test cases/suites we depend on */
};

typedef struct test_obj_s test_obj_t;

/* A test case. */
struct test_case_s
{
    int (*func) (struct test_case_s *); /* The unit test function */
    test_obj_t o;                       /* Test case attributes */
    pid_t pid;                          /* Process Id when exec'd in subproc */
    struct test_suite_s *pts;           /* Parent test suite */
};

/* A test suite. */
struct test_suite_s
{
    TO test_cases;      /* Head of TCs list */
    test_obj_t o;       /* Test suite attributes */
    struct test_s *pt;  /* Parent test */
};

/* A test. */
struct test_s
{
    unsigned int currank;   /* Current TS rank when sequencing */
    char id[TEST_ID_MAX];   /* Test id */
    TO test_suites;         /* Head of TSs list */
    char outfn[4096];       /* Output file name */
    time_t start, stop;     /* Test begin/end timestamps */
    _Bool sandboxed;        /* True if TC's exec'd in subproc */
    int max_parallel;       /* ... */
    _Bool verbose;          /* If true, be chatty */

    /* Test report hook functions */
    test_rep_f t_rep_cb;
    test_suite_rep_f ts_rep_cb;
    test_case_rep_f tc_rep_cb;
};

/* Get test suite handle by its '.o(bject)' field. */
#define TS_HANDLE(optr)    \
    (test_suite_t *) ((char *) optr - offsetof(test_suite_t, o))

/* Get test case handle by its '.o(bject)' field. */
#define TC_HANDLE(optr) \
    (test_case_t *) ((char *) optr - offsetof(test_case_t, o))


/* Test objects routines. */
static int test_obj_add (test_obj_t *to, TO *parent);
static int test_obj_dep_add (test_dep_t *td, test_obj_t *to);
static int test_obj_depends_on (const char *id, const char *depid, TO *parent);
static int test_obj_evict_id (TO *h, const char *id);
static int test_obj_init (const char *id, test_what_t what, test_obj_t *to);
static int test_obj_scheduler (TO *h, int (*sched)(test_obj_t *));
static int test_obj_sequencer (TO *h);
static test_obj_t *test_obj_pick_top (TO *h);
static test_obj_t *test_obj_search (TO *h, const char *id);
static void test_obj_free (test_obj_t *to);
static void test_obj_print (char nindent, test_obj_t *to);

/* Test dependencies routines. */
static int test_dep_new (const char *id, test_dep_t **ptd);
static test_dep_t *test_dep_search (TD *h, const char *id);
static void test_dep_free (test_dep_t *td);
static void test_dep_print (char nindent, test_dep_t *td);

/* Test case routines. */
static void test_case_print (test_case_t *tc);
static int test_case_dep_add (test_dep_t *td, test_case_t *tc);
static int test_case_scheduler (test_obj_t *to);
static int test_case_report_txt (FILE *fp, test_case_t *tc);
static int test_case_exec (test_case_t *tc);

/* Test cases routines. */
static int test_cases_reap (TO *h);
static test_case_t *test_cases_search_by_pid (TO *h, pid_t pid);

/* Test suite routines. */
static int test_suite_scheduler (test_obj_t *to);
static void test_suite_print (test_suite_t *ts);
static int test_suite_dep_add (test_dep_t *td, test_suite_t *ts);
static int test_suite_report_txt (FILE *fp, test_suite_t *ts, 
        test_rep_tag_t tag);

/* Test routines. */
static int test_sequencer (test_t *t);
static int test_scheduler (test_t *t);
static void test_print (test_t *t);
static int test_getopt (int ac, char *av[], test_t *t);
static void test_usage (const char *msg);
static int test_set_outfmt (test_t *t, const char *fmt);
static int test_reporter (test_t *t);
static int test_rep_txt (FILE *fp, test_t *t, test_rep_tag_t tag);

int test_run (int ac, char *av[], test_t *t)
{
//    test_print(t);

    con_err_if (test_getopt(ac, av, t));
    con_err_if (test_sequencer(t));
    con_err_if (test_scheduler(t));
    con_err_if (test_reporter(t));

    return 0;
err:
    return ~0;
}

int test_suite_dep_register (const char *id, test_suite_t *ts)
{
    test_dep_t *td = NULL;

    dbg_err_if (test_dep_new(id, &td));
    dbg_err_if (test_suite_dep_add(td, ts));

    return 0;
err:
    test_dep_free(td);
    return ~0;
}

int test_case_depends_on (const char *tcid, const char *depid, test_suite_t *ts)
{
    return test_obj_depends_on(tcid, depid, &ts->test_cases);
}

int test_suite_depends_on (const char *tsid, const char *depid, test_t *t)
{
    return test_obj_depends_on(tsid, depid, &t->test_suites);
}

int test_case_dep_register (const char *id, test_case_t *tc)
{
    test_dep_t *td = NULL;

    dbg_err_if (test_dep_new(id, &td));
    dbg_err_if (test_case_dep_add(td, tc));

    return 0;
err:
    test_dep_free(td);
    return ~0;
}

int test_case_register (const char *id, test_f func, test_suite_t *ts)
{
    test_case_t *tc = NULL;

    dbg_err_if (test_case_new(id, func, &tc));
    dbg_err_if (test_case_add(tc, ts));

    return 0;
err:
    test_case_free(tc);
    return ~0;
}

int test_suite_new (const char *id, test_suite_t **pts)
{
    test_suite_t *ts = NULL;

    dbg_return_if (id == NULL, ~0);
    dbg_return_if (pts == NULL, ~0);

    dbg_err_sif ((ts = u_malloc(sizeof *ts)) == NULL);
    LIST_INIT(&ts->test_cases.objs);
    ts->test_cases.currank = 0;

    dbg_err_if (test_obj_init(id, TEST_SUITE_T, &ts->o));

    *pts = ts;

    return 0;
err:
    test_suite_free(ts);
    return ~0;
}

void test_suite_free (test_suite_t *ts)
{
    test_case_t *tc;
    test_obj_t *to, *nto;

    if (ts == NULL)
        return;

    /* Free deps (should be empty in a sequenced suite). */
    test_obj_free(&ts->o);

    /* Free test cases. */
    LIST_FOREACH_SAFE (to, &ts->test_cases.objs, links, nto)
    {
        tc = TC_HANDLE(to);
        test_case_free(tc);
    }

    u_free(ts);

    return;
}

void test_free (test_t *t)
{
    test_suite_t *ts;
    test_obj_t *to, *nto;

    if (t == NULL)
        return;

    /* Free all children test suites. */
    LIST_FOREACH_SAFE (to, &t->test_suites.objs, links, nto)
    {
        ts = TS_HANDLE(to);
        test_suite_free(ts);
    } 

    /* Free the container. */
    u_free(t);

    return;
}

int test_set_test_rep (test_t *t, test_rep_f func)
{
    dbg_return_if (t == NULL, ~0);
    dbg_return_if (func == NULL, ~0);

    t->t_rep_cb = func;

    return 0;
}

int test_set_test_case_rep (test_t *t, test_case_rep_f func)
{
    dbg_return_if (t == NULL, ~0);
    dbg_return_if (func == NULL, ~0);

    t->tc_rep_cb = func;

    return 0;
}

int test_set_test_suite_rep (test_t *t, test_suite_rep_f func)
{
    dbg_return_if (t == NULL, ~0);
    dbg_return_if (func == NULL, ~0);

    t->ts_rep_cb = func;

    return 0;
}

int test_set_outfn (test_t *t, const char *outfn)
{
    dbg_return_if (t == NULL, ~0);
    dbg_return_if (outfn == NULL, ~0);

    dbg_return_if (u_strlcpy(t->outfn, outfn, sizeof t->outfn), ~0);

    return 0;
}

int test_new (const char *id, test_t **pt)
{
    test_t *t = NULL;

    dbg_return_if (pt == NULL, ~0);

    dbg_err_sif ((t = u_malloc(sizeof *t)) == NULL);

    LIST_INIT(&t->test_suites.objs);
    t->test_suites.currank = 0;
    dbg_err_if (u_strlcpy(t->id, id, sizeof t->id));
    t->currank = 0;
    t->start = t->stop = -1;
    (void) u_strlcpy(t->outfn, TEST_OUTFN_DFL, sizeof t->outfn);
    t->sandboxed = true;    /* the default is to spawn sub-processes to 
                               execute test cases */

    /* Set default report routines (may be overwritten). */
    t->t_rep_cb = test_rep_txt;
    t->ts_rep_cb = test_suite_report_txt;
    t->tc_rep_cb = test_case_report_txt;

    *pt = t;

    return 0;
err:
    u_free(t);
    return ~0;
}

void test_case_free (test_case_t *tc)
{
    if (tc == NULL)
        return;

    /* Free the attached dependencies (empty in a sequenced suite). */
    test_obj_free(&tc->o);

    /* Free the container. */
    u_free(tc);

    return;
}

int test_case_new (const char *id, test_f func, test_case_t **ptc)
{
    test_case_t *tc = NULL;

    dbg_return_if (id == NULL, ~0);
    dbg_return_if (ptc == NULL, ~0);

    dbg_err_sif ((tc = u_malloc(sizeof *tc)) == NULL);
    dbg_err_if (test_obj_init(id, TEST_CASE_T, &tc->o));

    tc->func = func;
    tc->pid = -1;

    *ptc = tc;

    return 0;
err:
    test_case_free(tc);
    return ~0;
}

int test_case_add (test_case_t *tc, test_suite_t *ts)
{
    dbg_return_if (tc == NULL, ~0);
    dbg_return_if (ts == NULL, ~0);

    LIST_INSERT_HEAD(&ts->test_cases.objs, &tc->o, links);
    tc->o.parent = &ts->test_cases;
    tc->pts = ts;

    return 0;
}

int test_suite_add (test_suite_t *ts, test_t *t)
{
    dbg_return_if (t == NULL, ~0);
    dbg_return_if (ts == NULL, ~0);

    LIST_INSERT_HEAD(&t->test_suites.objs, &ts->o, links);
    ts->o.parent = &t->test_suites;
    ts->pt = t;

    return 0;
}

void test_print (test_t *t)
{
    test_obj_t *to;

    dbg_return_if (t == NULL, );

    u_con("[test] %s", t->id);  

    LIST_FOREACH (to, &t->test_suites.objs, links)
        test_suite_print(TS_HANDLE(to));

    return;
}

static void test_suite_print (test_suite_t *ts)
{
    test_dep_t *td;
    test_obj_t *to;

    dbg_return_if (ts == NULL, );

    test_obj_print(4, &ts->o);

    LIST_FOREACH (td, &ts->o.deps, links)
        test_dep_print(4, td);

    LIST_FOREACH (to, &ts->test_cases.objs, links)
        test_case_print(TC_HANDLE(to));

    return;
}

static void test_case_print (test_case_t *tc)
{
    test_dep_t *td;

    dbg_return_if (tc == NULL, );

    test_obj_print(8, &tc->o);

    LIST_FOREACH (td, &tc->o.deps, links)
        test_dep_print(8, td);

    return;
}

static void test_obj_print (char nindent, test_obj_t *to)
{
    dbg_return_if (to == NULL, );

    u_con("%*c=> [%s] %s", 
            nindent, ' ', to->what == TEST_CASE_T ? "case" : "suite", to->id);

    /* attributes */
    u_con("%*c    .rank = %u", nindent, ' ', to->rank);  
    u_con("%*c    .seq = %s", nindent, ' ', to->sequenced ? "true" : "false");

    return;
}

static void test_dep_print (char nindent, test_dep_t *td)
{
    u_con("%*c    .<dep> = %s", nindent, ' ', td->id);
    return;
}

static int test_dep_new (const char *id, test_dep_t **ptd)
{
    test_dep_t *td = NULL;

    dbg_return_if (id == NULL, ~0);
    dbg_return_if (ptd == NULL, ~0);

    dbg_err_sif ((td = u_malloc(sizeof *td)) == NULL);
    dbg_err_if (u_strlcpy(td->id, id, sizeof td->id));

    *ptd = td;

    return 0;
err:
    test_dep_free(td);
    return ~0;
}

static void test_dep_free (test_dep_t *td)
{
    if (td)
        u_free(td);

    return;
}

static test_dep_t *test_dep_search (TD *h, const char *id)
{
    test_dep_t *td;

    dbg_return_if (h == NULL, NULL);
    dbg_return_if (id == NULL, NULL);

    LIST_FOREACH (td, h, links)
    {
        if (!strcmp(td->id, id))
            return td;
    }

    return NULL;
}

static test_obj_t *test_obj_search (TO *h, const char *id)
{
    test_obj_t *to;

    dbg_return_if (h == NULL, NULL);
    dbg_return_if (id == NULL, NULL);

    LIST_FOREACH (to, &h->objs, links)
    {
        if (!strcmp(to->id, id))
            return to;
    }

    return NULL;
}

static int test_obj_add (test_obj_t *to, TO *parent)
{
    dbg_return_if (to == NULL, ~0);
    dbg_return_if (parent == NULL, ~0);

    LIST_INSERT_HEAD(&parent->objs, to, links);
    to->parent = parent;

    return 0;
}

static int test_suite_dep_add (test_dep_t *td, test_suite_t *ts)
{
    return test_obj_dep_add(td, &ts->o);
}

static int test_case_dep_add (test_dep_t *td, test_case_t *tc)
{
    return test_obj_dep_add(td, &tc->o);
}

/* It MUST be called AFTER the test case/suite on which it is established 
 * has been added. */
static int test_obj_dep_add (test_dep_t *td, test_obj_t *to)
{
#if IMPLICIT_OBJ
    test_obj_t *nto = NULL;
    test_case_t *tc = NULL;
    test_suite_t *ts = NULL;
#endif
    
    dbg_return_if (to == NULL, ~0);
    dbg_return_if (td == NULL, ~0);

#if IMPLICIT_OBJ
    /* See if we have already added this test case/suite. 
     * In case it were not found, create it implicitly and stick it to the 
     * test cases/suites (depending on 'what') list. */
    if (test_obj_search(to->parent, to->id) == NULL)
    {
        switch (to->what) 
        {
            case TEST_CASE_T:
                dbg_err_sif (test_case_new(td->id, &tc));
                nto = &tc->o;
                break;
            case TEST_SUITE_T:
                dbg_err_sif (test_suite_new(td->id, &ts));
                nto = &ts->o;
                break;
        }

        dbg_err_if (test_obj_add(nto, to->parent));
        ts = NULL, tc = NULL;   /* ownership lost */
    }
#endif

    /* See if the dependency is not already been recorded, in which case
     * insert it into the dependency list for this test case/suite. */
    if (test_dep_search(&to->deps, to->id) == NULL)
    {
        LIST_INSERT_HEAD(&to->deps, td, links);
    }
    else
        u_free(td);

    return 0;
#if IMPLICIT_OBJ
err:
    if (ts)
        test_suite_free(ts);
    if (tc)
        test_case_free(tc);
    return ~0;
#endif
}

static void test_obj_free (test_obj_t *to)
{
    test_dep_t *td, *ntd;

    /* Just free the attached deps list: expect 'to' being a pointer to a
     * test object in a bigger malloc'd chunk (suite or case). */
    LIST_FOREACH_SAFE (td, &to->deps, links, ntd)
        test_dep_free(td); 

    return;
}

static test_obj_t *test_obj_pick_top (TO *h)
{
    /* Top element is a (not already sequenced) test object with no deps. */
    test_obj_t *to;

    LIST_FOREACH (to, &h->objs, links)
    {
        if (to->sequenced == false && LIST_EMPTY(&to->deps))
        {
            /* Record the reached depth: it will be used by the
             * next evicted element. */
            to->parent->currank = to->rank;
            return to;
        }
    }

    return NULL;
}

/* Do a (variation on) topological sorting over the partially ordered sets
 * of test suites/cases.  We are primarily concerned in equally ranking all
 * test objects which are at the same "dependency-depth" so that the scheduler
 * can exec tests in the same rank-pool in parallel.  We are also interested 
 * in creating a a partial order for cases where the scheduler is forced to 
 * run sequentially (i.e. no system fork(2) or by explicit user request).
 * In this respect, the ranking algorithm creates a partition of the 
 * case/suite's sets into equivalency classes from where any element can be 
 * picked at will (until EC exhaustion) to create a suitable sequence.
 * NOTE that higher ranks mean lower priority */
static int test_obj_sequencer (TO *h)
{
    test_obj_t *to, *fto;
    test_suite_t *ts;

    dbg_return_if (h == NULL, ~0);

    /* sequence test cases/suites */
    for (;;)
    {
        if ((to = test_obj_pick_top(h)) == NULL)
           break;

        dbg_err_if (test_obj_evict_id(h, to->id));
    }

    /* Test if we got out because of a cycle in deps, i.e. check if we 
     * still have any non-sequenced test case/suite. */
    LIST_FOREACH (to, &h->objs, links)
    {
        dbg_err_ifm (to->sequenced == false, 
                "%s not sequenced: dependency loop !", to->id);
    }

    /* A test suite need to recur into its test cases. */
    if ((fto = LIST_FIRST(&h->objs)) && fto->what == TEST_SUITE_T)
    {
        LIST_FOREACH (to, &h->objs, links)
        {
            ts = TS_HANDLE(to);
            dbg_err_if (test_obj_sequencer(&ts->test_cases));
        }
    }

    return 0;
err:
    return ~0;
}

static int test_obj_evict_id (TO *h, const char *id)
{
    test_obj_t *to;
    test_dep_t *td;

    dbg_return_if (id == NULL, ~0); 

    /* Delete from the deps list of every test suite, all the records
     * matching the given id. */
    LIST_FOREACH (to, &h->objs, links)
    {
        LIST_FOREACH (td, &to->deps, links)
        {
            if (!strcmp(td->id, id))
            {
                /* Set the rank of the evicted object to the actual
                 * depth +1 in the sequencing array */
                to->rank = to->parent->currank + 1;
                LIST_REMOVE(td, links); 
                test_dep_free(td);
                break;
            }
        }

        /* Eviction consists in asserting the '.sequenced' attribute of
         * the chosen test object. */
        if (!strcmp(to->id, id))
            to->sequenced = true;
    }

    return 0;
}

static int test_sequencer (test_t *t)
{
    dbg_return_if (t == NULL, ~0);

    return test_obj_sequencer(&t->test_suites);
}

static int test_obj_init (const char *id, test_what_t what, test_obj_t *to)
{
    dbg_return_if (id == NULL, ~0);
    dbg_return_if (to == NULL, ~0);

    LIST_INIT(&to->deps);
    to->what = what;
    to->parent = NULL;
    to->sequenced = false;
    to->rank = 0;
    to->status = TEST_SUCCESS;
    to->start = to->stop = -1;
    dbg_err_if (u_strlcpy(to->id, id, sizeof to->id)); 

    return 0;
err:
    return ~0;
}

static int test_obj_depends_on (const char *id, const char *depid, TO *parent)
{
    test_obj_t *to;
    test_dep_t *td = NULL;

    /* The object for which we are adding the dependency must be already
     * in place. */
    dbg_err_if ((to = test_obj_search(parent, id)) == NULL);

    /* Check if the same dependency is already recorded. */
    if ((td = test_dep_search(&to->deps, depid)))
        return 0;

    /* In case it is a new dependency, create and stick it to the object
     * deps list. */
    dbg_err_if (test_dep_new(depid, &td));
    dbg_err_if (test_obj_dep_add(td, to));

    return 0;
err:
    test_dep_free(td);
    return ~0;
}

static int test_obj_scheduler (TO *h, int (*sched)(test_obj_t *))
{
    _Bool simple_sched = false;
    unsigned int r, parallel = 0;
    test_obj_t *to, *fto, *tptr;
    test_case_t *ftc;

    dbg_return_if (h == NULL, ~0);
    dbg_return_if (sched == NULL, ~0);

    /* Pop the first element in list which will be used to reconnect to its 
     * upper layer relatives (i.e. test suite & test). */
    if ((fto = LIST_FIRST(&h->objs)) == NULL)
        return 0;   /* List empty, go out. */

    /* Initialize the spawned children (i.e. test cases) counter to 0.
     * It will be incremented on each test_case_exec() invocation, when 
     * in sandboxed mode. */
    h->nchildren = 0;

    /* See which kind of scheduler we need to use. */
    if (fto->what == TEST_SUITE_T)
        simple_sched = true;
    else
        simple_sched = (ftc = TC_HANDLE(fto), ftc->pts->pt->sandboxed == true)
            ? false : true;

    /* Go through all the test objs and select those at the current scheduling 
     * rank: first lower rank TO's (i.e. highest priority). */
    for (r = 0; r <= h->currank; ++r)
    {
        /* TS's and non-sandboxed TC's are simply scheduled one-by-one. */
        if (simple_sched == true)
        {
            LIST_FOREACH (to, &h->objs, links)
            {
                if (to->rank == r)
                    (void) sched(to);
            }
        }
        else    
        {
            /* Sandboxed TC's scheduling logic is slightly more complex:
             * we run them in chunks of TEST_MAX_PARALLEL cardinality, then
             * we reap the whole chunk and start again with another one, until
             * the list has been traversed. 'tptr' is the pointer to the next 
             * TC to be run. */

            tptr = LIST_FIRST(&h->objs);

        resched:
            for (to = tptr; to != NULL; to = LIST_NEXT(to, links))
            {
                if (to->rank == r)
                {
                    /* Schedule a TC at the current rank. */
                    dbg_if (sched(to));
            
                    /* See if we reached this chunk's upper bound. */
                    if ((parallel += 1) == TEST_MAX_PARALLEL)
                    {
                        /* Save restart reference and jump to reap. */
                        tptr = LIST_NEXT(to, links);
                        goto reap;
                    }
                }
                    
                /* Make sure 'tptr' is unset, so that, in case we go out on 
                 * loop exhaustion, the reaper branch knows that we don't need
                 * to be restarted. */
                tptr = NULL;
            }

        reap:
            dbg_if (test_cases_reap(h));

            /* If 'tptr' was set, it means that we need to restart. */
            if (tptr != NULL)
            {
                parallel = 0;       /* Reset parallel counter. */
                goto resched;       /* Go to sched again. */
            }
        }
    }

    return 0;
}

static int test_cases_reap (TO *h)
{
    int status;
    pid_t child;
    test_case_t *tc;

    dbg_return_if (h == NULL, ~0);

    while (h->nchildren > 0)
    {
        if ((child = waitpid(-1, &status, 0)) == -1)
        {
            if (errno == EINTR)         /* Interrupted, try again */
                continue;
            else if (errno == ECHILD)   /* No more child processes */
                break;
            else                        /* THIS IS BAD */
                con_err_sifm (1, "waitpid failed badly, test aborted");
        }

        /* TODO check what drove us here (i.e. stop, term, cont) */

        if ((tc = test_cases_search_by_pid(h, child)) == NULL)
        {
            u_con("not a waited test case: %d", child);
            continue;
        }

        h->nchildren -= 1;
 
        /* Update test case stats. */
        tc->o.stop = time(NULL);
    }

    con_err_ifm (h->nchildren != 0, 
            "%u child(ren) still in wait !", h->nchildren);

    return 0;
err:
    return ~0;
}

static test_case_t *test_cases_search_by_pid (TO *h, pid_t pid)
{
    test_obj_t *to;
    test_case_t *tc;

    LIST_FOREACH (to, &h->objs, links)
    {
        if (tc = TC_HANDLE(to), tc->pid == pid)
            return tc;
    }

    return NULL;
}

static int test_scheduler (test_t *t)
{
    return test_obj_scheduler(&t->test_suites, test_suite_scheduler);
}

static int test_suite_scheduler (test_obj_t *to)
{
    int rc;
    test_obj_t *tco;
    test_suite_t *ts;

    dbg_return_if (to == NULL || (ts = TS_HANDLE(to)) == NULL, ~0);

    u_con("now scheduling test suite %s", ts->o.id);

    /* Record test suite begin timestamp. */
    ts->o.start = time(NULL);

    /* Go through children test cases. */
    rc = test_obj_scheduler(&ts->test_cases, test_case_scheduler);

    /* Record test suite end timestamp. */
    ts->o.stop = time(NULL);

    /* Update stats for reporting. */
    LIST_FOREACH (tco, &ts->test_cases.objs, links)
    {
        /* If any of our test cases failed, set the overall status to FAILURE */
        if (tco->status != TEST_SUCCESS)
        {
            ts->o.status = TEST_FAILURE;
            break;
        }
    }

    return rc;
}

static int test_case_scheduler (test_obj_t *to)
{
    dbg_return_if (to == NULL, ~0);

    /* TODO handle no-fork */

    return test_case_exec(TC_HANDLE(to));
}

static int test_case_exec (test_case_t *tc)
{
    int rc;
    pid_t pid;

    dbg_return_if (tc == NULL, ~0);

    if (tc->func == NULL)
        return 0;

    if (tc->pts->pt->sandboxed == false)
    {
        rc = tc->func(tc);
        /* TODO report */
        return 0;
    }

    switch (pid = fork())
    {
        case -1:
            u_warn("test case %s spawn failed: %s", tc->o.id, strerror(errno));
            return ~0;
        case 0:
            exit(tc->func(tc));
        default:
            break;
    }

    tc->pid = pid;                  /* Set test case process pid */
    tc->o.parent->nchildren += 1;   /* Tell the test_case's head we have one
                                       more child */

    u_con("<%d> started test case %s with pid %d",
            getpid(), tc->o.id, tc->pid);

    /* always return ok here, the reaper will know the test exit status */
    return 0;
}

static int test_reporter (test_t *t)
{
    FILE *fp;
    test_obj_t *tso, *tco;
    test_suite_t *ts;
    test_rep_f t_rep;
    test_case_rep_f tc_rep;
    test_suite_rep_f ts_rep;

    dbg_return_if (t == NULL, ~0);

    /* The three following tests are rather paranoid because the
     * setters explicitly check that the supplied function is != NULL. */
    dbg_return_if ((t_rep = t->t_rep_cb) == NULL, ~0);
    dbg_return_if ((ts_rep = t->ts_rep_cb) == NULL, ~0);
    dbg_return_if ((tc_rep = t->tc_rep_cb) == NULL, ~0);

    dbg_err_sif ((fp = fopen(t->outfn, "w")) == NULL);

    dbg_err_if (t_rep(fp, t, TEST_REP_HEAD));

    LIST_FOREACH (tso, &t->test_suites.objs, links)
    {
        ts = TS_HANDLE(tso);

        dbg_err_if (ts_rep(fp, ts, TEST_REP_HEAD));

        LIST_FOREACH (tco, &ts->test_cases.objs, links)
            dbg_err_if (tc_rep(fp, TC_HANDLE(tco)));

        dbg_err_if (ts_rep(fp, ts, TEST_REP_TAIL));
    }

    dbg_err_if (t_rep(fp, t, TEST_REP_TAIL));

    (void) fclose(fp);

    return 0;
err:
    return ~0;
}

static int test_rep_txt (FILE *fp, test_t *t, test_rep_tag_t tag)
{
    if (tag == TEST_REP_TAIL)
        return 0;

    (void) fprintf(fp, "%s\n", t->id);

    return 0;
}

static int test_suite_report_txt (FILE *fp, test_suite_t *ts, 
        test_rep_tag_t tag)
{
    char b[80], e[80];

    if (tag == TEST_REP_TAIL)
        return 0;

    (void) strftime(b, sizeof b, "%a %Y-%m-%d %H:%M:%S %Z", 
            localtime(&ts->o.start));

    (void) strftime(e, sizeof e, "%a %Y-%m-%d %H:%M:%S %Z", 
            localtime(&ts->o.stop));

    (void) fprintf(fp, "\t* [%s] %s\n", 
            ts->o.status == TEST_SUCCESS ? "PASS" : "FAIL", ts->o.id);
    (void) fprintf(fp, "\t       begin: %s\n", b);
    (void) fprintf(fp, "\t         end: %s\n", e);

    return 0;
}

static int test_case_report_txt (FILE *fp, test_case_t *tc)
{
    (void) fprintf(fp, "\t\t* [%s] %s\n",
            tc->o.status == TEST_SUCCESS ? "PASS" : "FAIL", tc->o.id);

    return 0;
}

static int test_getopt (int ac, char *av[], test_t *t)
{
    int c;

    dbg_return_if (t == NULL, ~0);

    while ((c = getopt(ac, av, "o:f:p:vsh")) != -1)
    {
        switch (c)
        {
            case 'o':   /* Output file. */
                (void) u_strlcpy(t->outfn, optarg, sizeof t->outfn);
                break;
            case 'f':   /* Output format. */
                if (test_set_outfmt(t, optarg))
                    test_usage("bad output format");
                break;
            case 'p':   /* Max number of test cases running in parallel. */
                if (u_atoi(optarg, &t->max_parallel))
                    test_usage("bad max parallel");
                break;
            case 'v':   /* Increment verbosity. */
                t->verbose = true;
                break;
            case 's':   /* Serialized (i.e. !sandboxed) test cases. */
                t->sandboxed = false;
                break;
            case 'h': default:
                test_usage(NULL);
                break;
        } 
    }

    /* TODO global check */
    
    return 0;
}

static void test_usage (const char *msg)
{
    /* TODO */
    exit(1);
}

static int test_set_outfmt (test_t *t, const char *fmt)
{
    return 0;
}
