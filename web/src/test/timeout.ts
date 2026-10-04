// Functional tests guard completion, not throughput. Under build contention a
// worker can go minutes without running; assertions still diagnose wrong state.
export const testTimeout = 15 * 60_000
