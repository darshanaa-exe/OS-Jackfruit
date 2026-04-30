# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Darshana S | PES1UG24CS913 |
| Purvi      | PES1UG24CS570 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 in a VM with Secure Boot **OFF**. WSL is not supported.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Prepare the Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

Create one writable rootfs copy per container you plan to run:

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

> Do not commit `rootfs-base/` or `rootfs-*` directories to your repository.

### Build

```bash
cd boilerplate
make
```

For the CI-safe compile check (no kernel headers or sudo required):

```bash
make -C boilerplate ci
```

### Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

### Start the Supervisor

In a dedicated terminal:

```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor stays alive in the foreground, managing all containers and the logging pipeline.

### Launch Containers

In a second terminal:

```bash
# Background container
sudo ./engine start alpha ./rootfs-alpha "sleep 1000" --soft-mib 48 --hard-mib 80

# Background container with scheduling priority
sudo ./engine start beta ./rootfs-beta "sleep 1000" --soft-mib 64 --hard-mib 96 --nice -5

# Foreground container (blocks until it exits)
sudo ./engine run myrun ./rootfs-alpha "/bin/sh -c 'echo hello'"
```

To run test workloads inside a container, copy the binary into the rootfs first:

```bash
cp ./memory_hog ./rootfs-alpha/
sudo ./engine start mem-test ./rootfs-alpha "./memory_hog" --soft-mib 5 --hard-mib 10
```

### CLI Commands

```bash
# List running and past containers
sudo ./engine ps

# View captured logs for a container
sudo ./engine logs alpha

# Stop a running container
sudo ./engine stop alpha
```

### Inspect Kernel Events

```bash
# View all kernel monitor events
dmesg | tail -30

# Filter for memory limit events
sudo dmesg | grep "SOFT LIMIT"
sudo dmesg | grep "HARD LIMIT"
```

### Teardown

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Send SIGTERM to the supervisor (Ctrl-C in its terminal, or)
sudo kill -TERM <supervisor-pid>

# Unload the kernel module
sudo rmmod monitor

# Clean build artifacts
make clean
```

---

## 3. Demo with Screenshots

### Screenshot 1 — Multi-Container Supervision

Two containers (`alpha` and `beta`) launched under a single supervisor process. The supervisor terminal on the left confirms both containers started and shows their host PIDs.

![Multi-container supervision](1.png)

---

### Screenshot 2 — Metadata Tracking (`ps`)

Output of `engine ps` showing three tracked containers (`alpha`, `beta`, `gamma`) with their PID, state, start time, and configured soft/hard memory limits in MiB.

![Metadata tracking via ps](2.png)

---

### Screenshot 3 — Bounded-Buffer Logging (pipeline evidence)

The supervisor terminal (left) shows `[PRODUCER]`, `[BUFFER PUSH]`, and `[CONSUMER]` trace lines confirming that log data moves through the bounded buffer. The client terminal (right) shows `engine logs log-demo` reading the captured output, and `ls -l logs/log-demo.log` confirming the file was written to disk.

![Bounded-buffer pipeline activity](3-2.png)

`engine logs` output showing timestamped lines from the container:

![Logs output](3.png)

---

### Screenshot 4 — CLI and IPC

A `start` command is issued from a separate client process. The supervisor receives the request over the UNIX domain socket control channel, spawns the container, and returns a confirmation. The `ps` command immediately reflects the new container in the metadata table.

![CLI command and supervisor IPC response](4.png)

---

### Screenshots 5 & 6 — Soft-Limit Warning and Hard-Limit Enforcement

**Soft-limit (5 MiB):** `dmesg` captures the `SOFT LIMIT` warning from the kernel module when the `memory_hog` container's RSS first exceeds the configured soft threshold.

**Hard-limit (20 MiB):** The supervisor terminal prints `[HARD LIMIT] container=soft-test exceeded hard limit -> killing` followed by `[KILLED]`. The `engine ps` output shows the container state updated to `hard_limit_killed`.

![Soft and hard limit enforcement](5and6.png)

A second run confirming both soft and hard limit `dmesg` lines and `ps` reflecting `hard_limit_killed`:

![Hard limit kill confirmed in ps](6.png)

---

### Screenshots 7 & 7-1 — Scheduling Experiment

Two CPU-bound containers (`high_pri` with `--nice -20`, `low_pri` with `--nice 19`) run simultaneously on the same core. The `top` snapshot shows the priority (NI) column and the resulting CPU share split. The second screenshot shows `top -b -n 2 -p <pids>` sampled twice, revealing that the high-priority container consistently receives a larger CPU share while the low-priority one is throttled.

![Scheduling experiment - top output](7.png)

![Scheduling experiment - top batch output](7-1.png)

---

### Screenshots 8-1 & 8-2 — Clean Teardown

After the supervisor receives `SIGTERM` (via `Ctrl-C`), `ps aux | grep engine` returns no engine supervisor process. `ps aux | grep -i defunct` returns nothing, confirming zero zombie processes. `ls /tmp/mini_runtime.sock` confirms the control socket was removed. The supervisor terminal shows all containers were reaped before exit.

![Clean teardown - no zombies, socket removed](8-1.png)

![Clean teardown - supervisor terminal](8-2.png)

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

The runtime achieves process and filesystem isolation using Linux namespaces and `chroot`. When the supervisor spawns a container, it calls `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS`. These flags request three distinct kernel namespaces:

- **PID namespace:** The container's init process gets PID 1 inside its namespace. It cannot see or signal host processes. The kernel maintains a separate PID number space per namespace, mapping the container's PID 1 to a host PID that the supervisor tracks.
- **UTS namespace:** The container gets its own hostname and domain name, isolating `uname` results so workloads do not observe the host's identity.
- **Mount namespace:** A private copy of the mount table is given to the container. Mounts made inside (like `/proc`) do not propagate to the host.

After `clone()`, the child calls `chroot()` into its assigned `rootfs-*` directory before executing the workload. From that point the container's `/` is entirely within the overlay copy of the Alpine rootfs. The kernel still enforces `chroot` as a filesystem view restriction, not a security boundary — a privileged process with `CAP_SYS_CHROOT` could escape — but for this project it provides the required filesystem isolation. `pivot_root` would be more thorough but `chroot` is simpler and sufficient here.

Inside the container, `mount("proc", "/proc", "proc", 0, NULL)` attaches a new `procfs` instance, so tools like `ps` work correctly without leaking host process information through the inherited mount namespace.

What the host kernel still shares with all containers: the kernel itself (system calls, scheduler, memory allocator, device drivers), the network stack (unless a network namespace is added), the host clock, and the uid/gid space (unless a user namespace is used). Our runtime does not add network or user namespaces, so containers share the host network interfaces and run as root on the host.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is essential for two reasons. First, only the direct parent of a process can call `waitpid()` to reap it — if the parent exits, the child becomes an orphan adopted by `init` (PID 1), which removes our ability to track exit status. Second, the supervisor owns the control socket, log files, and container metadata that must persist across multiple `engine start` client invocations.

Process creation uses `clone()` rather than `fork()+exec()` because `clone()` lets us specify namespace flags atomically at creation time. The child entry function (`child_fn`) runs inside the new namespaces, performs `chroot`, mounts `/proc`, and `exec`s the workload. The supervisor retains the host PID returned by `clone()` and stores it in a `container_record_t` node protected by a `pthread_mutex_t metadata_lock`.

`SIGCHLD` is handled by the supervisor. The handler calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children without blocking. For each reaped PID the supervisor looks up the matching container record, sets its final state (`exited`, `stopped`, or `hard_limit_killed` depending on `stop_requested` and the signal), records the exit code, and unregisters the PID from the kernel module.

`SIGTERM`/`SIGINT` to the supervisor triggers an orderly shutdown: it sends `SIGTERM` to all still-running children, sets the bounded buffer's `shutting_down` flag, waits for the logger thread to join, closes the control socket, and removes the socket file.

### 4.3 IPC, Threads, and Synchronization

The project uses two IPC mechanisms with distinct roles:

**Path A — Logging (pipes):** Each container's stdout and stderr file descriptors are replaced inside `child_fn` with the write end of a pipe created before `clone()`. The supervisor holds the read end. A dedicated producer thread per container reads from the pipe and pushes `log_item_t` structs into a shared `bounded_buffer_t`. A single consumer thread pops items and appends them to per-container log files.

**Path B — Control (UNIX domain socket):** A `SOCK_STREAM` UNIX socket at `/tmp/mini_runtime.sock` connects short-lived client processes to the supervisor. The supervisor listens in its main event loop, reads a `control_request_t` struct, acts on it, and writes back a `control_response_t`. This is a separate mechanism from the logging pipes so that control flow is never mixed with log data. The socket is connection-oriented, providing natural framing and reliable delivery.

---

#### Why we chose these synchronization primitives

**Bounded buffer — `pthread_mutex_t` + two `pthread_cond_t` (`not_full`, `not_empty`):**
The bounded buffer is a fixed-size circular array with three shared fields: `head`, `tail`, and `count`. Multiple producer threads and the consumer thread access these fields concurrently. A mutex ensures that only one thread modifies the buffer state at a time, making each push or pop atomic from the perspective of other threads.

Condition variables are the right companion primitive because both producers and the consumer need to *sleep* when the buffer is in an undesirable state (full for producers, empty for the consumer) and be *woken up* when the state changes. A mutex alone cannot do this — a thread would have to busy-poll, holding or re-acquiring the lock repeatedly, wasting CPU and potentially starving the other side. Condition variables let the OS park the waiting thread and wake it precisely when progress is possible.

A semaphore pair could replace the two condition variables, but using `pthread_cond_t` alongside the mutex that already protects `head`/`tail`/`count` keeps both the state and the wait condition under a single lock, making the invariants easier to reason about and audit.

**Container metadata list — `pthread_mutex_t metadata_lock`:**
The linked list of `container_record_t` nodes is accessed from three concurrent contexts: the supervisor's main event loop (handling CLI requests), the `SIGCHLD` handler (updating final state after a child exits), and any thread responding to `engine ps`. A mutex is appropriate here because the critical section involves multi-step linked-list traversal and struct field updates — operations that are too coarse-grained for a lock-free approach and too long to hold a spinlock (spinlocks busy-wait, burning a CPU core for the entire duration, which is only acceptable in interrupt context where sleeping is forbidden).

---

#### What race conditions exist without these primitives

**Without the mutex on the bounded buffer:**
- **Torn read-modify-write on `count`:** Two producers could each read `count = 14`, both conclude the buffer is not full (capacity 16), both increment `count` to 15, and both write into slot 14 — one write is silently overwritten and that log line is permanently lost.
- **Stale `head`/`tail` visibility:** On architectures with weak memory ordering, one thread's write to `tail` may not be visible to another thread reading `tail` without a memory barrier. The mutex acquire/release provides the required barrier.
- **ABA / partial struct write:** A producer writing the `data`, `length`, and `container_id` fields of a `log_item_t` is not atomic. Without the mutex, a consumer could read a partially written item — seeing the new `length` but the previous iteration's `data` — producing a corrupted log line.

**Without the mutex on the metadata list:**
- **Use-after-free:** The `SIGCHLD` handler could `free()` a `container_record_t` node (after marking it exited) at the same moment the event loop is traversing the list to respond to `engine ps`, causing a read from freed memory.
- **Broken list linkage:** If two threads simultaneously insert or remove nodes, pointer updates to `->next` can interleave and corrupt the list structure, causing an infinite loop or a null-pointer dereference on the next traversal.

**Without condition variables on the bounded buffer:**
- **Missed wakeup:** If a producer checks `count == CAPACITY`, decides to wait, and the consumer decrements `count` and signals *before* the producer actually goes to sleep, the producer misses the signal and sleeps indefinitely — a deadlock. `pthread_cond_wait` atomically releases the mutex and enters the wait, closing this window.

---

#### How the bounded buffer avoids lost data, corruption, and deadlock

**Lost data:** The consumer thread does not exit immediately when `shutting_down` is set. It continues popping and writing until the buffer is fully drained (`count == 0`), only then returning. This guarantees that any log line a container wrote before exiting, which a producer already pushed into the buffer, will be flushed to disk before the logger thread joins.

**Corruption:** Every push and pop holds the mutex for the entire duration of the operation — copying the `log_item_t` into or out of the circular array slot, then updating `head`/`tail`/`count`. No partial state is ever visible to another thread.

**Deadlock:** Three conditions are addressed:
1. *Lock ordering:* There is only one lock on the buffer (`mutex`), so no lock-ordering cycle is possible between buffer locks.
2. *Condition-variable atomicity:* `pthread_cond_wait` releases the mutex before sleeping and reacquires it before returning, so the mutex is never held while the thread is parked — other threads can always make progress.
3. *Shutdown broadcast:* When `shutting_down` is set, `pthread_cond_broadcast` is called on *both* `not_empty` and `not_full`. This wakes all producers blocked on a full buffer and the consumer blocked on an empty buffer, so no thread sleeps forever waiting for a condition that will never become true.

### 4.4 Memory Management and Enforcement

RSS (Resident Set Size) measures the number of physical RAM pages currently mapped and present in a process's page tables — that is, pages that would need to be evicted if memory pressure increased. RSS does not measure: pages swapped out to disk, pages mapped but never faulted in (allocated but untouched), shared library pages counted once per library but attributed to every process mapping them, or kernel memory consumed on behalf of the process.

Soft and hard limits represent different enforcement policies. The soft limit is an advisory threshold: when RSS first exceeds it, the kernel module logs a warning but takes no action. This gives the workload a chance to shed memory (e.g., free a cache) before the hard limit is reached. The hard limit is a mandatory ceiling: exceeding it causes `SIGKILL`. The two-tier design avoids abruptly killing a container for a brief spike while still guaranteeing that runaway allocation is eventually terminated.

Enforcement belongs in kernel space rather than only in user space for two reasons. First, a user-space monitor cannot atomically observe and act on another process's memory usage — between reading `/proc/<pid>/status` and sending a signal, the process may have already allocated more pages. The kernel module's timer callback holds the kernel lock while checking RSS, giving a consistent snapshot. Second, a misbehaving or malicious container process could interfere with a user-space enforcer (e.g., by masking signals or outrunning polling), but it cannot prevent the kernel module from delivering `SIGKILL` through `send_sig()`.

### 4.5 Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) assigns CPU time proportional to each task's weight, which is derived from its `nice` value. The `nice` range is -20 (highest priority, largest weight) to 19 (lowest priority, smallest weight). Weight doubles roughly every 5 nice levels.

In our experiment (Screenshots 7 and 7-1), `high_pri` ran with `--nice -20` and `low_pri` with `--nice 19`. Both containers ran identical CPU-bound busy loops. The `top` output shows the CPU split strongly favouring `high_pri`: at `nice -20` the weight is approximately 88761, while at `nice 19` it is 15. The ratio gives `high_pri` roughly 98% of available CPU time, leaving `low_pri` nearly starved. This demonstrates CFS's fairness property — it is fair among processes of equal weight but intentionally unfair across different weights, honouring the administrator's expressed priority preference.

CFS still provides eventual progress to `low_pri`: it was never fully blocked, and its virtual runtime slowly advanced, preventing permanent starvation. This distinguishes CFS from strict priority scheduling where a lower-priority task may never run while higher-priority tasks are runnable.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation

**Decision:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` (no network or user namespaces).

**Tradeoff:** Adding a network namespace would provide stronger isolation but requires extra setup (creating a `veth` pair and configuring routing inside the container) that is out of scope. Without it, containers share the host network stack.

**Justification:** The project's requirements focus on process, filesystem, and mount isolation. PID + UTS + mount namespaces satisfy all stated requirements without the additional complexity of network virtualisation.

---

### Supervisor Architecture

**Decision:** The supervisor is a single process with a main event loop handling the control socket, plus one producer thread per container and one shared consumer thread for logging.

**Tradeoff:** A single consumer thread simplifies synchronisation (one writer per log file) but means log writes are serialised. Under very high concurrent output from many containers, the consumer could become a bottleneck.

**Justification:** For the expected workload scale (a few containers), a single consumer is simpler to reason about for correctness (especially for clean shutdown) and avoids per-file locking that multiple consumer threads would require.

---

### IPC and Logging

**Decision:** Pipes for log data (Path A) and a UNIX domain socket for control commands (Path B).

**Tradeoff:** A shared-memory ring buffer would have lower latency than pipes for logging, but would require explicit synchronisation between the container process and the supervisor. Pipes are provided by the kernel with built-in flow control and blocking semantics.

**Justification:** Pipes map naturally onto the producer-consumer model: the container writes to its stdout/stderr and the supervisor reads, with the kernel's pipe buffer absorbing bursts. The UNIX socket provides connection-oriented framing for control messages, making it easy to pair requests with responses.

---

### Kernel Monitor

**Decision:** Used a `mutex` (`DEFINE_MUTEX`) to protect the monitored entry linked list.

**Tradeoff:** A mutex can sleep, which means the timer callback may be delayed if the lock is contended. A spinlock would avoid sleeping but would busy-wait, burning CPU while the ioctl handler holds the lock.

**Justification:** The timer callback iterates the list and potentially frees entries — operations that may call `kfree`, which can sleep. `kfree` is called after `list_del` while the lock is held, but to be safe with future changes, a mutex is the right choice. The ioctl handler also runs in process context (not interrupt context), so sleeping is acceptable. The contention window is short (a few list operations per second), making the mutex cost negligible.

---

### Scheduling Experiments

**Decision:** Used `setpriority(PRIO_PROCESS, 0, nice_value)` inside the container's child process after `clone()` to apply the `--nice` value.

**Tradeoff:** `nice` adjusts CFS weight but does not provide hard CPU bandwidth guarantees. For strict CPU caps, `cgroups cpu.cfs_quota_us` would be more precise but requires cgroup setup.

**Justification:** The `nice`-based approach is directly observable through `top` (NI column) and is sufficient to demonstrate the scheduler's priority-proportional CPU allocation without the complexity of cgroup management.

---

## 6. Scheduler Experiment Results

### Experiment: CPU-Bound Workloads at Different Nice Values

Both containers ran `while true; do :; done` — a tight busy loop with no I/O or sleeps — making them purely CPU-bound and maximally sensitive to scheduling priority.

| Container | Nice Value | Observed CPU % (top snapshot) |
|-----------|-----------|-------------------------------|
| `high_pri` | -20 | ~50% of total (dominant share) |
| `low_pri`  | +19 | ~0–1% of total |

**Raw `top` output (Screenshot 7):**

```
PID    USER  PR  NI   VIRT   RES  SHR S  %CPU  %MEM   TIME+  COMMAND
10327  root   0 -20  ...              R  ~50    ...          sh
10344  root  39  19  ...              R   ~0    ...          sh
```

*(Exact values visible in Screenshot 7-1 batch output)*

### Analysis

CFS assigns CPU time proportional to scheduler weight. At `nice -20` the weight is 88761; at `nice 19` it is 15. The ratio is approximately 5917:1, meaning `high_pri` is entitled to nearly all available CPU time when both are runnable. The observed output matches this: `high_pri` consumed essentially the entire core while `low_pri` made negligible forward progress during the sampling window.

This illustrates two CFS properties:

1. **Priority-proportional fairness:** CPU time is not split 50/50 when weights differ. The scheduler is intentionally biased in favour of the higher-weight task.
2. **No starvation guarantee violation:** `low_pri` was never completely blocked — CFS tracks virtual runtime for every task and will eventually schedule even a very low-weight task to prevent starvation, but on a single-core machine the scheduling quantum for `low_pri` is extremely infrequent.

The experiment confirms that the `--nice` flag in the container CLI translates correctly into kernel-level CFS weights, and that the supervisor applies priority settings before `exec`-ing the workload inside the container.
