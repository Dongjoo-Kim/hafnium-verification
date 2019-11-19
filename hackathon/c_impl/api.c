/*
 * Copyright 2018 The Hafnium Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "hf/api.h"

#include "hf/arch/cpu.h"
#include "hf/arch/timer.h"

#include "hf/check.h"
#include "hf/dlog.h"
#include "hf/mm.h"
#include "hf/plat/console.h"
#include "hf/spci_internal.h"
#include "hf/spinlock.h"
#include "hf/static_assert.h"
#include "hf/std.h"
#include "hf/vm.h"

#include "vmapi/hf/call.h"
#include "vmapi/hf/spci.h"

/*
 * To eliminate the risk of deadlocks, we define a partial order for the
 * acquisition of locks held concurrently by the same physical CPU. Our current
 * ordering requirements are as follows:
 *
 * vm::lock -> vcpu::lock -> mm_stage1_lock -> dlog sl
 *
 * Locks of the same kind require the lock of lowest address to be locked first,
 * see `sl_lock_both()`.
 */

static_assert(HF_MAILBOX_SIZE == PAGE_SIZE,
	      "Currently, a page is mapped for the send and receive buffers so "
	      "the maximum request is the size of a page.");

static struct mpool api_page_pool;

/**
 * Initialises the API page pool by taking ownership of the contents of the
 * given page pool.
 */
void api_init(struct mpool *ppool)
{
	mpool_init_from(&api_page_pool, ppool);
}

/**
 * Switches the physical CPU back to the corresponding vcpu of the primary VM.
 *
 * This triggers the scheduling logic to run. Run in the context of secondary VM
 * to cause SPCI_RUN to return and the primary VM to regain control of the CPU.
 */
static struct vcpu *api_switch_to_primary(struct vcpu *current,
					  struct spci_value primary_ret,
					  enum vcpu_state secondary_state)
{
	struct vm *primary = vm_find(HF_PRIMARY_VM_ID);
	struct vcpu *next = vm_get_vcpu(primary, cpu_index(current->cpu));

	/*
	 * If the secondary is blocked but has a timer running, sleep until the
	 * timer fires rather than indefinitely.
	 */
	switch (primary_ret.func) {
	case HF_SPCI_RUN_WAIT_FOR_INTERRUPT:
	case SPCI_MSG_WAIT_32: {
		if (arch_timer_enabled_current()) {
			uint64_t remaining_ns =
				arch_timer_remaining_ns_current();

			if (remaining_ns == 0) {
				/*
				 * Timer is pending, so the current vCPU should
				 * be run again right away.
				 */
				primary_ret.func = SPCI_INTERRUPT_32;
				/*
				 * primary_ret.arg1 should already be set to the
				 * current VM ID and vCPU ID.
				 */
				primary_ret.arg2 = 0;
			} else {
				primary_ret.arg2 = remaining_ns;
			}
		} else {
			primary_ret.arg2 = SPCI_SLEEP_INDEFINITE;
		}
		break;
	}

	default:
		/* Do nothing. */
		break;
	}

	/* Set the return value for the primary VM's call to HF_VCPU_RUN. */
	arch_regs_set_retval(&next->regs, primary_ret);

	/* Mark the current vcpu as waiting. */
	sl_lock(&current->lock);
	current->state = secondary_state;
	sl_unlock(&current->lock);

	return next;
}

/**
 * Returns to the primary vm and signals that the vcpu still has work to do so.
 */
struct vcpu *api_preempt(struct vcpu *current)
{
	struct spci_value ret = {
		.func = SPCI_INTERRUPT_32,
		.arg1 = spci_vm_vcpu(current->vm->id, vcpu_index(current)),
	};

	return api_switch_to_primary(current, ret, VCPU_STATE_READY);
}

/**
 * Puts the current vcpu in wait for interrupt mode, and returns to the primary
 * vm.
 */
struct vcpu *api_wait_for_interrupt(struct vcpu *current)
{
	struct spci_value ret = {
		.func = HF_SPCI_RUN_WAIT_FOR_INTERRUPT,
		.arg1 = spci_vm_vcpu(current->vm->id, vcpu_index(current)),
	};

	return api_switch_to_primary(current, ret,
				     VCPU_STATE_BLOCKED_INTERRUPT);
}

/**
 * Puts the current vCPU in off mode, and returns to the primary VM.
 */
struct vcpu *api_vcpu_off(struct vcpu *current)
{
	struct spci_value ret = {
		.func = HF_SPCI_RUN_WAIT_FOR_INTERRUPT,
		.arg1 = spci_vm_vcpu(current->vm->id, vcpu_index(current)),
	};

	/*
	 * Disable the timer, so the scheduler doesn't get told to call back
	 * based on it.
	 */
	arch_timer_disable_current();

	return api_switch_to_primary(current, ret, VCPU_STATE_OFF);
}

/**
 * Returns to the primary vm to allow this cpu to be used for other tasks as the
 * vcpu does not have work to do at this moment. The current vcpu is marked as
 * ready to be scheduled again.
 */
void api_yield(struct vcpu *current, struct vcpu **next)
{
	struct spci_value primary_ret = {
		.func = SPCI_YIELD_32,
		.arg1 = spci_vm_vcpu(current->vm->id, vcpu_index(current)),
	};

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/* Noop on the primary as it makes the scheduling decisions. */
		return;
	}

	*next = api_switch_to_primary(current, primary_ret, VCPU_STATE_READY);
}

/**
 * Switches to the primary so that it can switch to the target, or kick it if it
 * is already running on a different physical CPU.
 */
struct vcpu *api_wake_up(struct vcpu *current, struct vcpu *target_vcpu)
{
	struct spci_value ret = {
		.func = HF_SPCI_RUN_WAKE_UP,
		.arg1 = spci_vm_vcpu(target_vcpu->vm->id,
				     vcpu_index(target_vcpu)),
	};
	return api_switch_to_primary(current, ret, VCPU_STATE_READY);
}

/**
 * Aborts the vCPU and triggers its VM to abort fully.
 */
struct vcpu *api_abort(struct vcpu *current)
{
	struct spci_value ret = spci_error(SPCI_ABORTED);

	dlog("Aborting VM %u vCPU %u\n", current->vm->id, vcpu_index(current));

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/* TODO: what to do when the primary aborts? */
		for (;;) {
			/* Do nothing. */
		}
	}

	atomic_store_explicit(&current->vm->aborting, true,
			      memory_order_relaxed);

	/* TODO: free resources once all vCPUs abort. */

	return api_switch_to_primary(current, ret, VCPU_STATE_ABORTED);
}

/**
 * Returns the ID of the VM.
 */
struct spci_value api_spci_id_get(const struct vcpu *current)
{
	return (struct spci_value){.func = SPCI_SUCCESS_32,
				   .arg2 = current->vm->id};
}

/**
 * Returns the number of VMs configured to run.
 */
spci_vm_count_t api_vm_get_count(void)
{
	return vm_get_count();
}

/**
 * Returns the number of vCPUs configured in the given VM, or 0 if there is no
 * such VM or the caller is not the primary VM.
 */
spci_vcpu_count_t api_vcpu_get_count(spci_vm_id_t vm_id,
				     const struct vcpu *current)
{
	struct vm *vm;

	/* Only the primary VM needs to know about vcpus for scheduling. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		return 0;
	}

	vm = vm_find(vm_id);
	if (vm == NULL) {
		return 0;
	}

	return vm->vcpu_count;
}

/**
 * This function is called by the architecture-specific context switching
 * function to indicate that register state for the given vcpu has been saved
 * and can therefore be used by other pcpus.
 */
void api_regs_state_saved(struct vcpu *vcpu)
{
	sl_lock(&vcpu->lock);
	vcpu->regs_available = true;
	sl_unlock(&vcpu->lock);
}

/**
 * Retrieves the next waiter and removes it from the wait list if the VM's
 * mailbox is in a writable state.
 */
static struct wait_entry *api_fetch_waiter(struct vm_locked locked_vm)
{
	struct wait_entry *entry;
	struct vm *vm = locked_vm.vm;

	if (vm->mailbox.state != MAILBOX_STATE_EMPTY ||
	    vm->mailbox.recv == NULL || list_empty(&vm->mailbox.waiter_list)) {
		/* The mailbox is not writable or there are no waiters. */
		return NULL;
	}

	/* Remove waiter from the wait list. */
	entry = CONTAINER_OF(vm->mailbox.waiter_list.next, struct wait_entry,
			     wait_links);
	list_remove(&entry->wait_links);
	return entry;
}

/**
 * Assuming that the arguments have already been checked by the caller, injects
 * a virtual interrupt of the given ID into the given target vCPU. This doesn't
 * cause the vCPU to actually be run immediately; it will be taken when the vCPU
 * is next run, which is up to the scheduler.
 *
 * Returns:
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick the target vCPU.
 */
static int64_t internal_interrupt_inject(struct vcpu *target_vcpu,
					 uint32_t intid, struct vcpu *current,
					 struct vcpu **next)
{
	uint32_t intid_index = intid / INTERRUPT_REGISTER_BITS;
	uint32_t intid_mask = 1U << (intid % INTERRUPT_REGISTER_BITS);
	int64_t ret = 0;

	sl_lock(&target_vcpu->lock);

	/*
	 * We only need to change state and (maybe) trigger a virtual IRQ if it
	 * is enabled and was not previously pending. Otherwise we can skip
	 * everything except setting the pending bit.
	 *
	 * If you change this logic make sure to update the need_vm_lock logic
	 * above to match.
	 */
	if (!(target_vcpu->interrupts.interrupt_enabled[intid_index] &
	      ~target_vcpu->interrupts.interrupt_pending[intid_index] &
	      intid_mask)) {
		goto out;
	}

	/* Increment the count. */
	target_vcpu->interrupts.enabled_and_pending_count++;

	/*
	 * Only need to update state if there was not already an
	 * interrupt enabled and pending.
	 */
	if (target_vcpu->interrupts.enabled_and_pending_count != 1) {
		goto out;
	}

	if (current->vm->id == HF_PRIMARY_VM_ID) {
		/*
		 * If the call came from the primary VM, let it know that it
		 * should run or kick the target vCPU.
		 */
		ret = 1;
	} else if (current != target_vcpu && next != NULL) {
		*next = api_wake_up(current, target_vcpu);
	}

out:
	/* Either way, make it pending. */
	target_vcpu->interrupts.interrupt_pending[intid_index] |= intid_mask;

	sl_unlock(&target_vcpu->lock);

	return ret;
}

/**
 * Constructs an SPCI_MSG_SEND value to return from a successful SPCI_MSG_POLL
 * or SPCI_MSG_WAIT call.
 */
static struct spci_value spci_msg_recv_return(const struct vm *receiver)
{
	return (struct spci_value){
		.func = SPCI_MSG_SEND_32,
		.arg1 = (receiver->mailbox.recv_sender << 16) | receiver->id,
		.arg3 = receiver->mailbox.recv_size,
		.arg4 = receiver->mailbox.recv_attributes};
}

/**
 * Prepares the vcpu to run by updating its state and fetching whether a return
 * value needs to be forced onto the vCPU.
 */
static bool api_vcpu_prepare_run(const struct vcpu *current, struct vcpu *vcpu,
				 struct spci_value *run_ret)
{
	bool need_vm_lock;
	bool ret;

	/*
	 * Wait until the registers become available. All locks must be released
	 * between iterations of this loop to avoid potential deadlocks if, on
	 * any path, a lock needs to be taken after taking the decision to
	 * switch context but before the registers have been saved.
	 *
	 * The VM lock is not needed in the common case so it must only be taken
	 * when it is going to be needed. This ensures there are no inter-vCPU
	 * dependencies in the common run case meaning the sensitive context
	 * switch performance is consistent.
	 */
	for (;;) {
		sl_lock(&vcpu->lock);

		/* The VM needs to be locked to deliver mailbox messages. */
		need_vm_lock = vcpu->state == VCPU_STATE_BLOCKED_MAILBOX;
		if (need_vm_lock) {
			sl_unlock(&vcpu->lock);
			sl_lock(&vcpu->vm->lock);
			sl_lock(&vcpu->lock);
		}

		if (vcpu->regs_available) {
			break;
		}

		if (vcpu->state == VCPU_STATE_RUNNING) {
			/*
			 * vCPU is running on another pCPU.
			 *
			 * It's okay not to return the sleep duration here
			 * because the other physical CPU that is currently
			 * running this vCPU will return the sleep duration if
			 * needed.
			 */
			*run_ret = spci_error(SPCI_BUSY);
			ret = false;
			goto out;
		}

		sl_unlock(&vcpu->lock);
		if (need_vm_lock) {
			sl_unlock(&vcpu->vm->lock);
		}
	}

	if (atomic_load_explicit(&vcpu->vm->aborting, memory_order_relaxed)) {
		if (vcpu->state != VCPU_STATE_ABORTED) {
			dlog("Aborting VM %u vCPU %u\n", vcpu->vm->id,
			     vcpu_index(vcpu));
			vcpu->state = VCPU_STATE_ABORTED;
		}
		ret = false;
		goto out;
	}

	switch (vcpu->state) {
	case VCPU_STATE_RUNNING:
	case VCPU_STATE_OFF:
	case VCPU_STATE_ABORTED:
		ret = false;
		goto out;

	case VCPU_STATE_BLOCKED_MAILBOX:
		/*
		 * A pending message allows the vCPU to run so the message can
		 * be delivered directly.
		 */
		if (vcpu->vm->mailbox.state == MAILBOX_STATE_RECEIVED) {
			arch_regs_set_retval(&vcpu->regs,
					     spci_msg_recv_return(vcpu->vm));
			vcpu->vm->mailbox.state = MAILBOX_STATE_READ;
			break;
		}
		/* Fall through. */
	case VCPU_STATE_BLOCKED_INTERRUPT:
		/* Allow virtual interrupts to be delivered. */
		if (vcpu->interrupts.enabled_and_pending_count > 0) {
			break;
		}

		if (arch_timer_enabled(&vcpu->regs)) {
			uint64_t timer_remaining_ns =
				arch_timer_remaining_ns(&vcpu->regs);

			/*
			 * The timer expired so allow the interrupt to be
			 * delivered.
			 */
			if (timer_remaining_ns == 0) {
				break;
			}

			/*
			 * The vCPU is not ready to run, return the appropriate
			 * code to the primary which called vcpu_run.
			 */
			run_ret->func =
				vcpu->state == VCPU_STATE_BLOCKED_MAILBOX
					? SPCI_MSG_WAIT_32
					: HF_SPCI_RUN_WAIT_FOR_INTERRUPT;
			run_ret->arg1 =
				spci_vm_vcpu(vcpu->vm->id, vcpu_index(vcpu));
			run_ret->arg2 = timer_remaining_ns;
		}

		ret = false;
		goto out;

	case VCPU_STATE_READY:
		break;
	}

	/* It has been decided that the vCPU should be run. */
	vcpu->cpu = current->cpu;
	vcpu->state = VCPU_STATE_RUNNING;

	/*
	 * Mark the registers as unavailable now that we're about to reflect
	 * them onto the real registers. This will also prevent another physical
	 * CPU from trying to read these registers.
	 */
	vcpu->regs_available = false;

	ret = true;

out:
	sl_unlock(&vcpu->lock);
	if (need_vm_lock) {
		sl_unlock(&vcpu->vm->lock);
	}

	return ret;
}

struct spci_value api_spci_run(spci_vm_id_t vm_id, spci_vcpu_index_t vcpu_idx,
			       const struct vcpu *current, struct vcpu **next)
{
	struct vm *vm;
	struct vcpu *vcpu;
	struct spci_value ret = spci_error(SPCI_INVALID_PARAMETERS);

	/* Only the primary VM can switch vcpus. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		ret.arg2 = SPCI_DENIED;
		goto out;
	}

	/* Only secondary VM vcpus can be run. */
	if (vm_id == HF_PRIMARY_VM_ID) {
		goto out;
	}

	/* The requested VM must exist. */
	vm = vm_find(vm_id);
	if (vm == NULL) {
		goto out;
	}

	/* The requested vcpu must exist. */
	if (vcpu_idx >= vm->vcpu_count) {
		goto out;
	}

	/* Update state if allowed. */
	vcpu = vm_get_vcpu(vm, vcpu_idx);
	if (!api_vcpu_prepare_run(current, vcpu, &ret)) {
		goto out;
	}

	/*
	 * Inject timer interrupt if timer has expired. It's safe to access
	 * vcpu->regs here because api_vcpu_prepare_run already made sure that
	 * regs_available was true (and then set it to false) before returning
	 * true.
	 */
	if (arch_timer_pending(&vcpu->regs)) {
		/* Make virtual timer interrupt pending. */
		internal_interrupt_inject(vcpu, HF_VIRTUAL_TIMER_INTID, vcpu,
					  NULL);

		/*
		 * Set the mask bit so the hardware interrupt doesn't fire
		 * again. Ideally we wouldn't do this because it affects what
		 * the secondary vCPU sees, but if we don't then we end up with
		 * a loop of the interrupt firing each time we try to return to
		 * the secondary vCPU.
		 */
		arch_timer_mask(&vcpu->regs);
	}

	/* Switch to the vcpu. */
	*next = vcpu;

	/*
	 * Set a placeholder return code to the scheduler. This will be
	 * overwritten when the switch back to the primary occurs.
	 */
	ret.func = SPCI_INTERRUPT_32;
	ret.arg1 = spci_vm_vcpu(vm_id, vcpu_idx);
	ret.arg2 = 0;

out:
	return ret;
}

/**
 * Check that the mode indicates memory that is valid, owned and exclusive.
 */
static bool api_mode_valid_owned_and_exclusive(uint32_t mode)
{
	return (mode & (MM_MODE_D | MM_MODE_INVALID | MM_MODE_UNOWNED |
			MM_MODE_SHARED)) == 0;
}

/**
 * Determines the value to be returned by api_vm_configure and spci_rx_release
 * after they've succeeded. If a secondary VM is running and there are waiters,
 * it also switches back to the primary VM for it to wake waiters up.
 */
static struct spci_value api_waiter_result(struct vm_locked locked_vm,
					   struct vcpu *current,
					   struct vcpu **next)
{
	struct vm *vm = locked_vm.vm;

	if (list_empty(&vm->mailbox.waiter_list)) {
		/* No waiters, nothing else to do. */
		return (struct spci_value){.func = SPCI_SUCCESS_32};
	}

	if (vm->id == HF_PRIMARY_VM_ID) {
		/* The caller is the primary VM. Tell it to wake up waiters. */
		return (struct spci_value){.func = SPCI_RX_RELEASE_32};
	}

	/*
	 * Switch back to the primary VM, informing it that there are waiters
	 * that need to be notified.
	 */
	*next = api_switch_to_primary(
		current, (struct spci_value){.func = SPCI_RX_RELEASE_32},
		VCPU_STATE_READY);

	return (struct spci_value){.func = SPCI_SUCCESS_32};
}

/**
 * Configures the hypervisor's stage-1 view of the send and receive pages. The
 * stage-1 page tables must be locked so memory cannot be taken by another core
 * which could result in this transaction being unable to roll back in the case
 * of an error.
 */
static bool api_vm_configure_stage1(struct vm_locked vm_locked,
				    paddr_t pa_send_begin, paddr_t pa_send_end,
				    paddr_t pa_recv_begin, paddr_t pa_recv_end,
				    struct mpool *local_page_pool)
{
	bool ret;
	struct mm_stage1_locked mm_stage1_locked = mm_lock_stage1();

	/* Map the send page as read-only in the hypervisor address space. */
	vm_locked.vm->mailbox.send =
		mm_identity_map(mm_stage1_locked, pa_send_begin, pa_send_end,
				MM_MODE_R, local_page_pool);
	if (!vm_locked.vm->mailbox.send) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(mm_stage1_locked, local_page_pool);
		goto fail;
	}

	/*
	 * Map the receive page as writable in the hypervisor address space. On
	 * failure, unmap the send page before returning.
	 */
	vm_locked.vm->mailbox.recv =
		mm_identity_map(mm_stage1_locked, pa_recv_begin, pa_recv_end,
				MM_MODE_W, local_page_pool);
	if (!vm_locked.vm->mailbox.recv) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(mm_stage1_locked, local_page_pool);
		goto fail_undo_send;
	}

	ret = true;
	goto out;

	/*
	 * The following mappings will not require more memory than is available
	 * in the local pool.
	 */
fail_undo_send:
	vm_locked.vm->mailbox.send = NULL;
	CHECK(mm_unmap(mm_stage1_locked, pa_send_begin, pa_send_end,
		       local_page_pool));

fail:
	ret = false;

out:
	mm_unlock_stage1(&mm_stage1_locked);

	return ret;
}

/**
 * Configures the send and receive pages in the VM stage-2 and hypervisor
 * stage-1 page tables. Locking of the page tables combined with a local memory
 * pool ensures there will always be enough memory to recover from any errors
 * that arise.
 */
static bool api_vm_configure_pages(struct vm_locked vm_locked,
				   paddr_t pa_send_begin, paddr_t pa_send_end,
				   uint32_t orig_send_mode,
				   paddr_t pa_recv_begin, paddr_t pa_recv_end,
				   uint32_t orig_recv_mode)
{
	bool ret;
	struct mpool local_page_pool;

	/*
	 * Create a local pool so any freed memory can't be used by another
	 * thread. This is to ensure the original mapping can be restored if any
	 * stage of the process fails.
	 */
	mpool_init_with_fallback(&local_page_pool, &api_page_pool);

	/* Take memory ownership away from the VM and mark as shared. */
	if (!mm_vm_identity_map(
		    &vm_locked.vm->ptable, pa_send_begin, pa_send_end,
		    MM_MODE_UNOWNED | MM_MODE_SHARED | MM_MODE_R | MM_MODE_W,
		    NULL, &local_page_pool)) {
		goto fail;
	}

	if (!mm_vm_identity_map(&vm_locked.vm->ptable, pa_recv_begin,
				pa_recv_end,
				MM_MODE_UNOWNED | MM_MODE_SHARED | MM_MODE_R,
				NULL, &local_page_pool)) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_vm_defrag(&vm_locked.vm->ptable, &local_page_pool);
		goto fail_undo_send;
	}

	if (!api_vm_configure_stage1(vm_locked, pa_send_begin, pa_send_end,
				     pa_recv_begin, pa_recv_end,
				     &local_page_pool)) {
		goto fail_undo_send_and_recv;
	}

	ret = true;
	goto out;

	/*
	 * The following mappings will not require more memory than is available
	 * in the local pool.
	 */
fail_undo_send_and_recv:
	CHECK(mm_vm_identity_map(&vm_locked.vm->ptable, pa_recv_begin,
				 pa_recv_end, orig_recv_mode, NULL,
				 &local_page_pool));

fail_undo_send:
	CHECK(mm_vm_identity_map(&vm_locked.vm->ptable, pa_send_begin,
				 pa_send_end, orig_send_mode, NULL,
				 &local_page_pool));

fail:
	ret = false;

out:
	mpool_fini(&local_page_pool);

	return ret;
}

/**
 * Configures the VM to send/receive data through the specified pages. The pages
 * must not be shared.
 *
 * Returns:
 *  - SPCI_ERROR SPCI_INVALID_PARAMETERS if the given addresses are not properly
 *    aligned or are the same.
 *  - SPCI_ERROR SPCI_NO_MEMORY if the hypervisor was unable to map the buffers
 *    due to insuffient page table memory.
 *  - SPCI_ERROR SPCI_DENIED if the pages are already mapped or are not owned by
 *    the caller.
 *  - SPCI_SUCCESS on success if no further action is needed.
 *  - SPCI_RX_RELEASE if it was called by the primary VM and the primary VM now
 *    needs to wake up or kick waiters.
 */
struct spci_value api_spci_rxtx_map(ipaddr_t send, ipaddr_t recv,
				    uint32_t page_count, struct vcpu *current,
				    struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct vm_locked vm_locked;
	paddr_t pa_send_begin;
	paddr_t pa_send_end;
	paddr_t pa_recv_begin;
	paddr_t pa_recv_end;
	uint32_t orig_send_mode;
	uint32_t orig_recv_mode;
	struct spci_value ret;

	/* Hafnium only supports a fixed size of RX/TX buffers. */
	if (page_count != HF_MAILBOX_SIZE / SPCI_PAGE_SIZE) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Fail if addresses are not page-aligned. */
	if (!is_aligned(ipa_addr(send), PAGE_SIZE) ||
	    !is_aligned(ipa_addr(recv), PAGE_SIZE)) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Convert to physical addresses. */
	pa_send_begin = pa_from_ipa(send);
	pa_send_end = pa_add(pa_send_begin, HF_MAILBOX_SIZE);

	pa_recv_begin = pa_from_ipa(recv);
	pa_recv_end = pa_add(pa_recv_begin, HF_MAILBOX_SIZE);

	/* Fail if the same page is used for the send and receive pages. */
	if (pa_addr(pa_send_begin) == pa_addr(pa_recv_begin)) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/*
	 * The hypervisor's memory map must be locked for the duration of this
	 * operation to ensure there will be sufficient memory to recover from
	 * any failures.
	 *
	 * TODO: the scope of the can be reduced but will require restructuring
	 *       to keep a single unlock point.
	 */
	vm_locked = vm_lock(vm);

	/* We only allow these to be setup once. */
	if (vm->mailbox.send || vm->mailbox.recv) {
		ret = spci_error(SPCI_DENIED);
		goto exit;
	}

	/*
	 * Ensure the pages are valid, owned and exclusive to the VM and that
	 * the VM has the required access to the memory.
	 */
	if (!mm_vm_get_mode(&vm->ptable, send, ipa_add(send, PAGE_SIZE),
			    &orig_send_mode) ||
	    !api_mode_valid_owned_and_exclusive(orig_send_mode) ||
	    (orig_send_mode & MM_MODE_R) == 0 ||
	    (orig_send_mode & MM_MODE_W) == 0) {
		ret = spci_error(SPCI_DENIED);
		goto exit;
	}

	if (!mm_vm_get_mode(&vm->ptable, recv, ipa_add(recv, PAGE_SIZE),
			    &orig_recv_mode) ||
	    !api_mode_valid_owned_and_exclusive(orig_recv_mode) ||
	    (orig_recv_mode & MM_MODE_R) == 0) {
		ret = spci_error(SPCI_DENIED);
		goto exit;
	}

	if (!api_vm_configure_pages(vm_locked, pa_send_begin, pa_send_end,
				    orig_send_mode, pa_recv_begin, pa_recv_end,
				    orig_recv_mode)) {
		ret = spci_error(SPCI_NO_MEMORY);
		goto exit;
	}

	/* Tell caller about waiters, if any. */
	ret = api_waiter_result(vm_locked, current, next);

exit:
	vm_unlock(&vm_locked);

	return ret;
}

/**
 * Checks whether the given `to` VM's mailbox is currently busy, and optionally
 * registers the `from` VM to be notified when it becomes available.
 */
static bool msg_receiver_busy(struct vm_locked to, struct vm_locked from,
			      bool notify)
{
	if (to.vm->mailbox.state != MAILBOX_STATE_EMPTY ||
	    to.vm->mailbox.recv == NULL) {
		/*
		 * Fail if the receiver isn't currently ready to receive data,
		 * setting up for notification if requested.
		 */
		if (notify) {
			struct wait_entry *entry =
				&from.vm->wait_entries[to.vm->id];

			/* Append waiter only if it's not there yet. */
			if (list_empty(&entry->wait_links)) {
				list_append(&to.vm->mailbox.waiter_list,
					    &entry->wait_links);
			}
		}

		return true;
	}

	return false;
}

/**
 * Notifies the `to` VM about the message currently in its mailbox, possibly
 * with the help of the primary VM.
 */
static void deliver_msg(struct vm_locked to, struct vm_locked from,
			uint32_t size, struct vcpu *current, struct vcpu **next)
{
	struct spci_value primary_ret = {
		.func = SPCI_MSG_SEND_32,
		.arg1 = ((uint32_t)from.vm->id << 16) | to.vm->id,
	};

	/* Messages for the primary VM are delivered directly. */
	if (to.vm->id == HF_PRIMARY_VM_ID) {
		/*
		 * Only tell the primary VM the size if the message is for it,
		 * to avoid leaking data about messages for other VMs.
		 */
		primary_ret.arg3 = size;

		to.vm->mailbox.state = MAILBOX_STATE_READ;
		*next = api_switch_to_primary(current, primary_ret,
					      VCPU_STATE_READY);
		return;
	}

	to.vm->mailbox.state = MAILBOX_STATE_RECEIVED;

	/* Return to the primary VM directly or with a switch. */
	if (from.vm->id != HF_PRIMARY_VM_ID) {
		*next = api_switch_to_primary(current, primary_ret,
					      VCPU_STATE_READY);
	}
}

/**
 * Copies data from the sender's send buffer to the recipient's receive buffer
 * and notifies the recipient.
 *
 * If the recipient's receive buffer is busy, it can optionally register the
 * caller to be notified when the recipient's receive buffer becomes available.
 */
struct spci_value api_spci_msg_send(spci_vm_id_t sender_vm_id,
				    spci_vm_id_t receiver_vm_id, uint32_t size,
				    uint32_t attributes, struct vcpu *current,
				    struct vcpu **next)
{
	struct vm *from = current->vm;
	struct vm *to;

	struct two_vm_locked vm_to_from_lock;

	const void *from_msg;

	struct spci_value ret;
	bool notify = (attributes & SPCI_MSG_SEND_NOTIFY_MASK) ==
		      SPCI_MSG_SEND_NOTIFY;

	/* Ensure sender VM ID corresponds to the current VM. */
	if (sender_vm_id != from->id) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Disallow reflexive requests as this suggests an error in the VM. */
	if (receiver_vm_id == from->id) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Limit the size of transfer. */
	if (size > SPCI_MSG_PAYLOAD_MAX) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/*
	 * Check that the sender has configured its send buffer. If the tx
	 * mailbox at from_msg is configured (i.e. from_msg != NULL) then it can
	 * be safely accessed after releasing the lock since the tx mailbox
	 * address can only be configured once.
	 */
	sl_lock(&from->lock);
	from_msg = from->mailbox.send;
	sl_unlock(&from->lock);

	if (from_msg == NULL) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Ensure the receiver VM exists. */
	to = vm_find(receiver_vm_id);
	if (to == NULL) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/*
	 * Hafnium needs to hold the lock on <to> before the mailbox state is
	 * checked. The lock on <to> must be held until the information is
	 * copied to <to> Rx buffer. Since in
	 * spci_msg_handle_architected_message we may call api_spci_share_memory
	 * which must hold the <from> lock, we must hold the <from> lock at this
	 * point to prevent a deadlock scenario.
	 */
	vm_to_from_lock = vm_lock_both(to, from);

	if (msg_receiver_busy(vm_to_from_lock.vm1, vm_to_from_lock.vm2,
			      notify)) {
		ret = spci_error(SPCI_BUSY);
		goto out;
	}

	/* Handle legacy memory sharing messages. */
	if ((attributes & SPCI_MSG_SEND_LEGACY_MEMORY_MASK) ==
	    SPCI_MSG_SEND_LEGACY_MEMORY) {
		/*
		 * Buffer holding the internal copy of the shared memory
		 * regions.
		 */
		struct spci_architected_message_header
			*architected_message_replica =
				(struct spci_architected_message_header *)
					cpu_get_buffer(current->cpu->id);
		uint32_t message_buffer_size =
			cpu_get_buffer_size(current->cpu->id);

		struct spci_architected_message_header *architected_header =
			(struct spci_architected_message_header *)from_msg;

		if (size > message_buffer_size) {
			ret = spci_error(SPCI_INVALID_PARAMETERS);
			goto out;
		}

		if (size < sizeof(struct spci_architected_message_header)) {
			ret = spci_error(SPCI_INVALID_PARAMETERS);
			goto out;
		}

		/* Copy the architected message into the internal buffer. */
		memcpy_s(architected_message_replica, message_buffer_size,
			 architected_header, size);

		/*
		 * Note that architected_message_replica is passed as the third
		 * parameter to spci_msg_handle_architected_message. The
		 * execution flow commencing at
		 * spci_msg_handle_architected_message will make several
		 * accesses to fields in architected_message_replica. The memory
		 * area architected_message_replica must be exclusively owned by
		 * Hafnium so that TOCTOU issues do not arise.
		 */
		ret = spci_msg_handle_architected_message(
			vm_to_from_lock.vm1, vm_to_from_lock.vm2,
			architected_message_replica, size);

		if (ret.func != SPCI_SUCCESS_32) {
			goto out;
		}
	} else {
		/* Copy data. */
		memcpy_s(to->mailbox.recv, SPCI_MSG_PAYLOAD_MAX, from_msg,
			 size);
		to->mailbox.recv_size = size;
		to->mailbox.recv_sender = sender_vm_id;
		to->mailbox.recv_attributes = 0;
		ret = (struct spci_value){.func = SPCI_SUCCESS_32};
	}

	deliver_msg(vm_to_from_lock.vm1, vm_to_from_lock.vm2, size, current,
		    next);

out:
	vm_unlock(&vm_to_from_lock.vm1);
	vm_unlock(&vm_to_from_lock.vm2);

	return ret;
}

/**
 * Checks whether the vCPU's attempt to block for a message has already been
 * interrupted or whether it is allowed to block.
 */
bool api_spci_msg_recv_block_interrupted(struct vcpu *current)
{
	bool interrupted;

	sl_lock(&current->lock);

	/*
	 * Don't block if there are enabled and pending interrupts, to match
	 * behaviour of wait_for_interrupt.
	 */
	interrupted = (current->interrupts.enabled_and_pending_count > 0);

	sl_unlock(&current->lock);

	return interrupted;
}

/**
 * Receives a message from the mailbox. If one isn't available, this function
 * can optionally block the caller until one becomes available.
 *
 * No new messages can be received until the mailbox has been cleared.
 */
struct spci_value api_spci_msg_recv(bool block, struct vcpu *current,
				    struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct spci_value return_code;

	/*
	 * The primary VM will receive messages as a status code from running
	 * vcpus and must not call this function.
	 */
	if (vm->id == HF_PRIMARY_VM_ID) {
		return spci_error(SPCI_NOT_SUPPORTED);
	}

	sl_lock(&vm->lock);

	/* Return pending messages without blocking. */
	if (vm->mailbox.state == MAILBOX_STATE_RECEIVED) {
		vm->mailbox.state = MAILBOX_STATE_READ;
		return_code = spci_msg_recv_return(vm);
		goto out;
	}

	/* No pending message so fail if not allowed to block. */
	if (!block) {
		return_code = spci_error(SPCI_RETRY);
		goto out;
	}

	/*
	 * From this point onward this call can only be interrupted or a message
	 * received. If a message is received the return value will be set at
	 * that time to SPCI_SUCCESS.
	 */
	return_code = spci_error(SPCI_INTERRUPTED);
	if (api_spci_msg_recv_block_interrupted(current)) {
		goto out;
	}

	/* Switch back to primary vm to block. */
	{
		struct spci_value run_return = {
			.func = SPCI_MSG_WAIT_32,
			.arg1 = spci_vm_vcpu(vm->id, vcpu_index(current)),
		};

		*next = api_switch_to_primary(current, run_return,
					      VCPU_STATE_BLOCKED_MAILBOX);
	}
out:
	sl_unlock(&vm->lock);

	return return_code;
}

/**
 * Retrieves the next VM whose mailbox became writable. For a VM to be notified
 * by this function, the caller must have called api_mailbox_send before with
 * the notify argument set to true, and this call must have failed because the
 * mailbox was not available.
 *
 * It should be called repeatedly to retrieve a list of VMs.
 *
 * Returns -1 if no VM became writable, or the id of the VM whose mailbox
 * became writable.
 */
int64_t api_mailbox_writable_get(const struct vcpu *current)
{
	struct vm *vm = current->vm;
	struct wait_entry *entry;
	int64_t ret;

	sl_lock(&vm->lock);
	if (list_empty(&vm->mailbox.ready_list)) {
		ret = -1;
		goto exit;
	}

	entry = CONTAINER_OF(vm->mailbox.ready_list.next, struct wait_entry,
			     ready_links);
	list_remove(&entry->ready_links);
	ret = entry - vm->wait_entries;

exit:
	sl_unlock(&vm->lock);
	return ret;
}

/**
 * Retrieves the next VM waiting to be notified that the mailbox of the
 * specified VM became writable. Only primary VMs are allowed to call this.
 *
 * Returns -1 on failure or if there are no waiters; the VM id of the next
 * waiter otherwise.
 */
int64_t api_mailbox_waiter_get(spci_vm_id_t vm_id, const struct vcpu *current)
{
	struct vm *vm;
	struct vm_locked locked;
	struct wait_entry *entry;
	struct vm *waiting_vm;

	/* Only primary VMs are allowed to call this function. */
	if (current->vm->id != HF_PRIMARY_VM_ID) {
		return -1;
	}

	vm = vm_find(vm_id);
	if (vm == NULL) {
		return -1;
	}

	/* Check if there are outstanding notifications from given vm. */
	locked = vm_lock(vm);
	entry = api_fetch_waiter(locked);
	vm_unlock(&locked);

	if (entry == NULL) {
		return -1;
	}

	/* Enqueue notification to waiting VM. */
	waiting_vm = entry->waiting_vm;

	sl_lock(&waiting_vm->lock);
	if (list_empty(&entry->ready_links)) {
		list_append(&waiting_vm->mailbox.ready_list,
			    &entry->ready_links);
	}
	sl_unlock(&waiting_vm->lock);

	return waiting_vm->id;
}

/**
 * Releases the caller's mailbox so that a new message can be received. The
 * caller must have copied out all data they wish to preserve as new messages
 * will overwrite the old and will arrive asynchronously.
 *
 * Returns:
 *  - SPCI_ERROR SPCI_DENIED on failure, if the mailbox hasn't been read.
 *  - SPCI_SUCCESS on success if no further action is needed.
 *  - SPCI_RX_RELEASE if it was called by the primary VM and the primary VM now
 *    needs to wake up or kick waiters. Waiters should be retrieved by calling
 *    hf_mailbox_waiter_get.
 */
struct spci_value api_spci_rx_release(struct vcpu *current, struct vcpu **next)
{
	struct vm *vm = current->vm;
	struct vm_locked locked;
	struct spci_value ret;

	locked = vm_lock(vm);
	switch (vm->mailbox.state) {
	case MAILBOX_STATE_EMPTY:
		ret = (struct spci_value){.func = SPCI_SUCCESS_32};
		break;

	case MAILBOX_STATE_RECEIVED:
		ret = spci_error(SPCI_DENIED);
		break;

	case MAILBOX_STATE_READ:
		ret = api_waiter_result(locked, current, next);
		vm->mailbox.state = MAILBOX_STATE_EMPTY;
		break;
	}
	vm_unlock(&locked);

	return ret;
}

/**
 * Enables or disables a given interrupt ID for the calling vCPU.
 *
 * Returns 0 on success, or -1 if the intid is invalid.
 */
int64_t api_interrupt_enable(uint32_t intid, bool enable, struct vcpu *current)
{
	uint32_t intid_index = intid / INTERRUPT_REGISTER_BITS;
	uint32_t intid_mask = 1U << (intid % INTERRUPT_REGISTER_BITS);

	if (intid >= HF_NUM_INTIDS) {
		return -1;
	}

	sl_lock(&current->lock);
	if (enable) {
		/*
		 * If it is pending and was not enabled before, increment the
		 * count.
		 */
		if (current->interrupts.interrupt_pending[intid_index] &
		    ~current->interrupts.interrupt_enabled[intid_index] &
		    intid_mask) {
			current->interrupts.enabled_and_pending_count++;
		}
		current->interrupts.interrupt_enabled[intid_index] |=
			intid_mask;
	} else {
		/*
		 * If it is pending and was enabled before, decrement the count.
		 */
		if (current->interrupts.interrupt_pending[intid_index] &
		    current->interrupts.interrupt_enabled[intid_index] &
		    intid_mask) {
			current->interrupts.enabled_and_pending_count--;
		}
		current->interrupts.interrupt_enabled[intid_index] &=
			~intid_mask;
	}

	sl_unlock(&current->lock);
	return 0;
}

/**
 * Returns the ID of the next pending interrupt for the calling vCPU, and
 * acknowledges it (i.e. marks it as no longer pending). Returns
 * HF_INVALID_INTID if there are no pending interrupts.
 */
uint32_t api_interrupt_get(struct vcpu *current)
{
	uint8_t i;
	uint32_t first_interrupt = HF_INVALID_INTID;

	/*
	 * Find the first enabled and pending interrupt ID, return it, and
	 * deactivate it.
	 */
	sl_lock(&current->lock);
	for (i = 0; i < HF_NUM_INTIDS / INTERRUPT_REGISTER_BITS; ++i) {
		uint32_t enabled_and_pending =
			current->interrupts.interrupt_enabled[i] &
			current->interrupts.interrupt_pending[i];

		if (enabled_and_pending != 0) {
			uint8_t bit_index = ctz(enabled_and_pending);
			/*
			 * Mark it as no longer pending and decrement the count.
			 */
			current->interrupts.interrupt_pending[i] &=
				~(1U << bit_index);
			current->interrupts.enabled_and_pending_count--;
			first_interrupt =
				i * INTERRUPT_REGISTER_BITS + bit_index;
			break;
		}
	}

	sl_unlock(&current->lock);
	return first_interrupt;
}

/**
 * Returns whether the current vCPU is allowed to inject an interrupt into the
 * given VM and vCPU.
 */
static inline bool is_injection_allowed(uint32_t target_vm_id,
					struct vcpu *current)
{
	uint32_t current_vm_id = current->vm->id;

	/*
	 * The primary VM is allowed to inject interrupts into any VM. Secondary
	 * VMs are only allowed to inject interrupts into their own vCPUs.
	 */
	return current_vm_id == HF_PRIMARY_VM_ID ||
	       current_vm_id == target_vm_id;
}

/**
 * Injects a virtual interrupt of the given ID into the given target vCPU.
 * This doesn't cause the vCPU to actually be run immediately; it will be taken
 * when the vCPU is next run, which is up to the scheduler.
 *
 * Returns:
 *  - -1 on failure because the target VM or vCPU doesn't exist, the interrupt
 *    ID is invalid, or the current VM is not allowed to inject interrupts to
 *    the target VM.
 *  - 0 on success if no further action is needed.
 *  - 1 if it was called by the primary VM and the primary VM now needs to wake
 *    up or kick the target vCPU.
 */
int64_t api_interrupt_inject(spci_vm_id_t target_vm_id,
			     spci_vcpu_index_t target_vcpu_idx, uint32_t intid,
			     struct vcpu *current, struct vcpu **next)
{
	struct vcpu *target_vcpu;
	struct vm *target_vm = vm_find(target_vm_id);

	if (intid >= HF_NUM_INTIDS) {
		return -1;
	}

	if (target_vm == NULL) {
		return -1;
	}

	if (target_vcpu_idx >= target_vm->vcpu_count) {
		/* The requested vcpu must exist. */
		return -1;
	}

	if (!is_injection_allowed(target_vm_id, current)) {
		return -1;
	}

	target_vcpu = vm_get_vcpu(target_vm, target_vcpu_idx);

	dlog("Injecting IRQ %d for VM %d VCPU %d from VM %d VCPU %d\n", intid,
	     target_vm_id, target_vcpu_idx, current->vm->id, current->cpu->id);
	return internal_interrupt_inject(target_vcpu, intid, current, next);
}

/**
 * Clears a region of physical memory by overwriting it with zeros. The data is
 * flushed from the cache so the memory has been cleared across the system.
 */
static bool api_clear_memory(paddr_t begin, paddr_t end, struct mpool *ppool)
{
	/*
	 * TODO: change this to a cpu local single page window rather than a
	 *       global mapping of the whole range. Such an approach will limit
	 *       the changes to stage-1 tables and will allow only local
	 *       invalidation.
	 */
	bool ret;
	struct mm_stage1_locked stage1_locked = mm_lock_stage1();
	void *ptr =
		mm_identity_map(stage1_locked, begin, end, MM_MODE_W, ppool);
	size_t size = pa_difference(begin, end);

	if (!ptr) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_defrag(stage1_locked, ppool);
		goto fail;
	}

	memset_s(ptr, size, 0, size);
	arch_mm_flush_dcache(ptr, size);
	mm_unmap(stage1_locked, begin, end, ppool);

	ret = true;
	goto out;

fail:
	ret = false;

out:
	mm_unlock_stage1(&stage1_locked);

	return ret;
}

/** TODO: Move function to spci_architected_message.c. */
/**
 * Shares memory from the calling VM with another. The memory can be shared in
 * different modes.
 *
 * This function requires the calling context to hold the <to> and <from> locks.
 *
 * Returns:
 *  In case of error one of the following values is returned:
 *   1) SPCI_INVALID_PARAMETERS - The endpoint provided parameters were
 *     erroneous;
 *   2) SPCI_NO_MEMORY - Hafnium did not have sufficient memory to complete
 *     the request.
 *  Success is indicated by SPCI_SUCCESS.
 */
struct spci_value api_spci_share_memory(
	struct vm_locked to_locked, struct vm_locked from_locked,
	struct spci_memory_region *memory_region, uint32_t memory_to_attributes,
	enum spci_memory_share share)
{
	struct vm *to = to_locked.vm;
	struct vm *from = from_locked.vm;
	uint32_t orig_from_mode;
	uint32_t from_mode;
	uint32_t to_mode;
	struct mpool local_page_pool;
	struct spci_value ret;
	paddr_t pa_begin;
	paddr_t pa_end;
	ipaddr_t begin;
	ipaddr_t end;
	struct spci_memory_region_constituent *constituents =
		spci_memory_region_get_constituents(memory_region);

	size_t size;

	/*
	 * Make sure constituents are properly aligned to a 64-bit boundary. If
	 * not we would get alignment faults trying to read (64-bit) page
	 * addresses.
	 */
	if (!is_aligned(constituents, 8)) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/* Disallow reflexive shares as this suggests an error in the VM. */
	if (to == from) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	/*
	 * Create a local pool so any freed memory can't be used by another
	 * thread. This is to ensure the original mapping can be restored if any
	 * stage of the process fails.
	 */
	mpool_init_with_fallback(&local_page_pool, &api_page_pool);

	/* Obtain the single contiguous set of pages from the memory_region. */
	/* TODO: Add support for multiple constituent regions. */
	size = constituents[0].page_count * PAGE_SIZE;
	begin = ipa_init(constituents[0].address);
	end = ipa_add(begin, size);

	/*
	 * Check if the state transition is lawful for both VMs involved
	 * in the memory exchange, ensure that all constituents of a memory
	 * region being shared are at the same state.
	 */
	if (!spci_msg_check_transition(to, from, share, &orig_from_mode, begin,
				       end, memory_to_attributes, &from_mode,
				       &to_mode)) {
		return spci_error(SPCI_INVALID_PARAMETERS);
	}

	pa_begin = pa_from_ipa(begin);
	pa_end = pa_from_ipa(end);

	/*
	 * First update the mapping for the sender so there is not overlap with
	 * the recipient.
	 */
	if (!mm_vm_identity_map(&from->ptable, pa_begin, pa_end, from_mode,
				NULL, &local_page_pool)) {
		ret = spci_error(SPCI_NO_MEMORY);
		goto out;
	}

	/* Complete the transfer by mapping the memory into the recipient. */
	if (!mm_vm_identity_map(&to->ptable, pa_begin, pa_end, to_mode, NULL,
				&local_page_pool)) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_vm_defrag(&from->ptable, &local_page_pool);

		ret = spci_error(SPCI_NO_MEMORY);

		CHECK(mm_vm_identity_map(&from->ptable, pa_begin, pa_end,
					 orig_from_mode, NULL,
					 &local_page_pool));

		goto out;
	}

	ret = (struct spci_value){.func = SPCI_SUCCESS_32};

out:
	mpool_fini(&local_page_pool);

	return ret;
}

/**
 * Shares memory from the calling VM with another. The memory can be shared in
 * different modes.
 *
 * TODO: the interface for sharing memory will need to be enhanced to allow
 *       sharing with different modes e.g. read-only, informing the recipient
 *       of the memory they have been given, opting to not wipe the memory and
 *       possibly allowing multiple blocks to be transferred. What this will
 *       look like is TBD.
 */
int64_t api_share_memory(spci_vm_id_t vm_id, ipaddr_t addr, size_t size,
			 enum hf_share share, struct vcpu *current)
{
	struct vm *from = current->vm;
	struct vm *to;
	uint32_t orig_from_mode;
	uint32_t from_mode;
	uint32_t to_mode;
	ipaddr_t begin;
	ipaddr_t end;
	paddr_t pa_begin;
	paddr_t pa_end;
	struct mpool local_page_pool;
	int64_t ret;

	/* Disallow reflexive shares as this suggests an error in the VM. */
	if (vm_id == from->id) {
		return -1;
	}

	/* Ensure the target VM exists. */
	to = vm_find(vm_id);
	if (to == NULL) {
		return -1;
	}

	begin = addr;
	end = ipa_add(addr, size);

	/* Fail if addresses are not page-aligned. */
	if (!is_aligned(ipa_addr(begin), PAGE_SIZE) ||
	    !is_aligned(ipa_addr(end), PAGE_SIZE)) {
		return -1;
	}

	/* Convert the sharing request to memory management modes. */
	switch (share) {
	case HF_MEMORY_GIVE:
		from_mode = MM_MODE_INVALID | MM_MODE_UNOWNED;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X;
		break;

	case HF_MEMORY_LEND:
		from_mode = MM_MODE_INVALID;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_UNOWNED;
		break;

	case HF_MEMORY_SHARE:
		from_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_SHARED;
		to_mode = MM_MODE_R | MM_MODE_W | MM_MODE_X | MM_MODE_UNOWNED |
			  MM_MODE_SHARED;
		break;

	default:
		/* The input is untrusted so might not be a valid value. */
		return -1;
	}

	/*
	 * Create a local pool so any freed memory can't be used by another
	 * thread. This is to ensure the original mapping can be restored if any
	 * stage of the process fails.
	 */
	mpool_init_with_fallback(&local_page_pool, &api_page_pool);

	sl_lock_both(&from->lock, &to->lock);

	/*
	 * Ensure that the memory range is mapped with the same mode so that
	 * changes can be reverted if the process fails.
	 */
	if (!mm_vm_get_mode(&from->ptable, begin, end, &orig_from_mode)) {
		goto fail;
	}

	/* Ensure the address range is normal memory and not a device. */
	if (orig_from_mode & MM_MODE_D) {
		goto fail;
	}

	/*
	 * Ensure the memory range is valid for the sender. If it isn't, the
	 * sender has either shared it with another VM already or has no claim
	 * to the memory.
	 */
	if (orig_from_mode & MM_MODE_INVALID) {
		goto fail;
	}

	/*
	 * The sender must own the memory and have exclusive access to it in
	 * order to share it. Alternatively, it is giving memory back to the
	 * owning VM.
	 */
	if (orig_from_mode & MM_MODE_UNOWNED) {
		uint32_t orig_to_mode;

		if (share != HF_MEMORY_GIVE ||
		    !mm_vm_get_mode(&to->ptable, begin, end, &orig_to_mode) ||
		    orig_to_mode & MM_MODE_UNOWNED) {
			goto fail;
		}
	} else if (orig_from_mode & MM_MODE_SHARED) {
		goto fail;
	}

	pa_begin = pa_from_ipa(begin);
	pa_end = pa_from_ipa(end);

	/*
	 * First update the mapping for the sender so there is not overlap with
	 * the recipient.
	 */
	if (!mm_vm_identity_map(&from->ptable, pa_begin, pa_end, from_mode,
				NULL, &local_page_pool)) {
		goto fail;
	}

	/* Clear the memory so no VM or device can see the previous contents. */
	if (!api_clear_memory(pa_begin, pa_end, &local_page_pool)) {
		goto fail_return_to_sender;
	}

	/* Complete the transfer by mapping the memory into the recipient. */
	if (!mm_vm_identity_map(&to->ptable, pa_begin, pa_end, to_mode, NULL,
				&local_page_pool)) {
		/* TODO: partial defrag of failed range. */
		/* Recover any memory consumed in failed mapping. */
		mm_vm_defrag(&from->ptable, &local_page_pool);
		goto fail_return_to_sender;
	}

	ret = 0;
	goto out;

fail_return_to_sender:
	CHECK(mm_vm_identity_map(&from->ptable, pa_begin, pa_end,
				 orig_from_mode, NULL, &local_page_pool));

fail:
	ret = -1;

out:
	sl_unlock(&from->lock);
	sl_unlock(&to->lock);

	mpool_fini(&local_page_pool);

	return ret;
}

/** Returns the version of the implemented SPCI specification. */
struct spci_value api_spci_version(void)
{
	/*
	 * Ensure that both major and minor revision representation occupies at
	 * most 15 bits.
	 */
	static_assert(0x8000 > SPCI_VERSION_MAJOR,
		      "Major revision representation take more than 15 bits.");
	static_assert(0x10000 > SPCI_VERSION_MINOR,
		      "Minor revision representation take more than 16 bits.");

	struct spci_value ret = {
		.func = SPCI_SUCCESS_32,
		.arg2 = (SPCI_VERSION_MAJOR << SPCI_VERSION_MAJOR_OFFSET) |
			SPCI_VERSION_MINOR};
	return ret;
}

int64_t api_debug_log(char c, struct vcpu *current)
{
	bool flush;
	struct vm *vm = current->vm;
	struct vm_locked vm_locked = vm_lock(vm);

	if (c == '\n' || c == '\0') {
		flush = true;
	} else {
		vm->log_buffer[vm->log_buffer_length++] = c;
		flush = (vm->log_buffer_length == sizeof(vm->log_buffer));
	}

	if (flush) {
		dlog_flush_vm_buffer(vm->id, vm->log_buffer,
				     vm->log_buffer_length);
		vm->log_buffer_length = 0;
	}

	vm_unlock(&vm_locked);

	return 0;
}

/**
 * Discovery function returning information about the implementation of optional
 * SPCI interfaces.
 */
struct spci_value api_spci_features(uint32_t function_id)
{
	switch (function_id) {
	case SPCI_ERROR_32:
	case SPCI_SUCCESS_32:
	case SPCI_ID_GET_32:
	case SPCI_YIELD_32:
	case SPCI_VERSION_32:
	case SPCI_FEATURES_32:
	case SPCI_MSG_SEND_32:
	case SPCI_MSG_POLL_32:
	case SPCI_MSG_WAIT_32:
		return (struct spci_value){.func = SPCI_SUCCESS_32};
	default:
		return spci_error(SPCI_NOT_SUPPORTED);
	}
}