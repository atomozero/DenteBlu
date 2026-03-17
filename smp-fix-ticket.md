# KDL panic in find_free_message when SMP message pool is exhausted under interrupt context

## Summary

`find_free_message()` in `src/system/kernel/smp.cpp` panics with `ASSERT(are_interrupts_enabled())` when the SMP message pool is temporarily exhausted and the caller has interrupts disabled. This is a legitimate code path that occurs when `release_sem_etc()` wakes a thread on a different CPU: `release_sem_etc` disables interrupts via `InterruptsLocker`, calls `thread_unblock_locked` → `scheduler_enqueue_in_run_queue` → `enqueue` → `smp_send_ici` → `find_free_message`. If `sFreeMessageCount` happens to be zero at that point, the assert fires.

## Reproduction

Observed on an 11th Gen Intel Core i7-1165G7 (Tiger Lake) with an Intel AX201 Bluetooth USB controller. The XHCI driver generates a burst of completion events that produce enough async ICI reschedule messages to temporarily exhaust the pool. The panic occurs both from the XHCI event thread (`XHCI::ProcessEvents` → `release_sem_etc`) and directly from the XHCI interrupt handler (`XHCI::Interrupt` → `release_sem_etc`).

Stack trace:

```
PANIC: ASSERT FAILED (../haiku-git/src/system/kernel/smp.cpp:667): are_interrupts_enabled()
Thread 1 "idle thread 1" running on CPU 0
  find_free_message(smp_msg**) + 0x4c
  smp_send_ici + 0x3f
  enqueue(BKernel::Thread*, bool) + 0x11d
  scheduler_enqueue_in_run_queue + 0xa2
  release_sem_etc + 0x3ed
  XHCI::Interrupt[clone .localias] () + 0x14b
  io_interrupt_handler + 0xab
  x86_hardware_interrupt + 0x121
  intr_bottom + 0x80
```

## Analysis

The spin-wait loop in `find_free_message` assumes interrupts are always enabled when it needs to wait, but `release_sem_etc` legitimately calls into the scheduler with interrupts disabled. When the pool has free messages the code works fine because the while-loop is skipped entirely. The assert only fires under transient pool exhaustion.

The same pattern of calling `process_all_pending_ici` with interrupts disabled is already used elsewhere in `smp.cpp`, for example inside `acquire_spinlock` (line 359).

## Proposed fix

When interrupts are disabled and the pool is empty, process pending ICIs on the current CPU to free async messages, instead of asserting. This is safe because `process_all_pending_ici` is designed to run with interrupts disabled.

```diff
--- a/src/system/kernel/smp.cpp
+++ b/src/system/kernel/smp.cpp
@@ -664,7 +664,10 @@ find_free_message(struct smp_msg** msg)

 retry:
 	while (sFreeMessageCount <= 0) {
-		ASSERT(are_interrupts_enabled());
-		cpu_pause();
+		if (are_interrupts_enabled()) {
+			cpu_pause();
+		} else {
+			process_all_pending_ici(smp_get_current_cpu());
+			cpu_pause();
+		}
 	}

 	state = disable_interrupts();
```
