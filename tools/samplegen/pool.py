"""Small process-pool helper with deterministic result ordering.

The pool size defaults to ``os.cpu_count() - 1`` (minimum 1) and can be overridden.
Results come back in the same order as the input items regardless of which worker
finished first, so callers can rely on a stable output ordering. With one job (or one
item) everything runs in the calling process, which keeps debugging simple.
"""

import multiprocessing
import os


def default_jobs():
    """Default worker count: all CPUs but one, never below one."""
    return max(1, (os.cpu_count() or 2) - 1)


def run_ordered(func, items, jobs=None):
    """Apply ``func`` to every item, in parallel, returning results in input order.

    ``func`` must be importable from a worker process (a module-level function).
    """
    items = list(items)
    if not items:
        return []
    if jobs is None:
        jobs = default_jobs()
    jobs = max(1, min(int(jobs), len(items)))
    if jobs == 1:
        return [func(item) for item in items]
    ctx = multiprocessing.get_context("spawn")
    with ctx.Pool(processes=jobs) as pool:
        return pool.map(func, items, chunksize=1)
