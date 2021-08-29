/*
 * Copyright 2021 The Chromium OS Authors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "smf.h"

#include <logging/log.h>
LOG_MODULE_REGISTER(smf);

/*
 * Private structure (to this file) used to track state machine context.
 * The structure is not used directly, but instead to cast the "internal"
 * member of the smf_ctx structure.
 */
struct internal_ctx {
	bool new_state : 1;
	bool terminate : 1;
	bool exit      : 1;
};

/**
 * @brief Execute all ancestor entry actions
 *
 * @param ctx State machine context
 * @param target The entry actions of this target's ancestors are executed
 * @return true if the state machine should terminate, else false
 */
__unused static bool smf_execute_ancestor_entry_actions(
		struct smf_ctx *const ctx, const struct smf_state *target)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;
	const struct smf_state *tmp_state[CONFIG_NUM_SMF_ANCESTORS];
	const struct smf_state *last_parent;

	/*
	 * Prepare to execute all entry actions of the target's
	 * parent states
	 */

	tmp_state[CONFIG_NUM_SMF_ANCESTORS - 1] = target->parent;

	/* Get all parent states of the target */
	for (int i = CONFIG_NUM_SMF_ANCESTORS - 1; i > 0; i--) {
		if (tmp_state[i] != NULL) {
			tmp_state[i - 1] = tmp_state[i]->parent;
		} else {
			tmp_state[i - 1] = NULL;
		}
	}

	/* Get parent state of previous state */
	if (ctx->previous) {
		last_parent = ctx->previous->parent;
	} else {
		last_parent = NULL;
	}

	/* Execute all parent state entry actions in forward order */
	for (int i = 0; i < CONFIG_NUM_SMF_ANCESTORS; i++) {
		/* No parent state */
		if (tmp_state[i] == NULL) {
			continue;
		}

		/*
		 * We only want to execute the target state's parent state
		 * entry action if it doesn't share a parent state with the
		 * previous state.
		 */
		while (last_parent != NULL) {
			if (tmp_state[i] == last_parent) {
				return false;
			}

			/* Get last state's next parent state if it exists */
			last_parent = last_parent->parent;
		}

		/* Execute parent state's entry */
		if (tmp_state[i]->entry) {
			tmp_state[i]->entry(ctx);

			/* No need to continue if terminate was set */
			if (internal->terminate) {
				return true;
			}
		}
	}
	return false;
}

/**
 * @brief Execute all ancestor run actions
 *
 * @param ctx State machine context
 * @param target The run actions of this target's ancestors are executed
 * @return true if the state machine should terminate, else false
 */
__unused static bool smf_execute_ancestor_run_actions(struct smf_ctx *ctx)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;
	const struct smf_state *tmp_state;

	/* Execute all run actions in reverse order */

	/* Return if the current state switched states */
	if (internal->new_state) {
		internal->new_state = false;
		return false;
	}

	/* Return if the current state terminated */
	if (internal->terminate) {
		return true;
	}

	/* Try to run parent run actions */
	tmp_state = ctx->current->parent;
	for (int i = 0; i < CONFIG_NUM_SMF_ANCESTORS; i++) {
		/* Execute parent run action */
		if (tmp_state && tmp_state->run) {
			tmp_state->run(ctx);
		}

		/* break if the parent state switched states */
		if (internal->new_state) {
			break;
		}

		/* No need to continue if terminate was set */
		if (internal->terminate) {
			return true;
		}

		/* Get next parent state */
		if (tmp_state) {
			tmp_state = tmp_state->parent;
		}
	}

	internal->new_state = false;
	/* All done executing the run actions */

	return false;
}

/**
 * @brief Execute all ancestor exit actions
 *
 * @param ctx State machine context
 * @param target The exit actions of this target's ancestors are executed
 * @return true if the state machine should terminate, else false
 */
__unused static bool smf_execute_ancestor_exit_actions(
		struct smf_ctx *const ctx, const struct smf_state *target)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;
	const struct smf_state *tmp_state;
	const struct smf_state *target_parent;

	/* Execute all parent exit actions in reverse order */

	/* Get target state's parent state */
	target_parent = target->parent;
	tmp_state = ctx->current;

	for (int i = 0; i < CONFIG_NUM_SMF_ANCESTORS; i++) {
		/* Get parent state */
		if (tmp_state == NULL) {
			break;
		}

		tmp_state = tmp_state->parent;

		/*
		 * Do not execute a parent state's exit action that has
		 * a shared ancestry with the target.
		 */
		while (target_parent != NULL) {
			if (tmp_state == target_parent) {
				tmp_state = NULL;
				break;
			}

			/* Get target state next parent state if it exists */
			target_parent = target_parent->parent;
		}

		if (tmp_state && tmp_state->exit) {
			/* Execute exit action */
			tmp_state->exit(ctx);

			/* No need to continue if terminate was set */
			if (internal->terminate) {
				return true;
			}
		}
	}
	return false;
}

void smf_set_initial(struct smf_ctx *ctx, const struct smf_state *init_state)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;

	internal->exit = false;
	internal->terminate = false;
	ctx->current = init_state;
	ctx->previous = NULL;
	ctx->terminate_val = 0;

	if (IS_ENABLED(CONFIG_SMF_ANCESTOR_SUPPORT)) {
		internal->new_state = false;

		if (smf_execute_ancestor_entry_actions(ctx, init_state)) {
			return;
		}
	}

	/* Now execute the initial state's entry action */
	if (init_state->entry) {
		init_state->entry(ctx);
	}
}


void smf_set_state(struct smf_ctx *const ctx, const struct smf_state *target)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;

	/*
	 * It does not make sense to call set_state in an exit phase of a state
	 * since we are already in a transition; we would always ignore the
	 * intended state to transition into.
	 */
	if (internal->exit) {
		LOG_WRN("Calling %s from exit action", __func__);
		return;
	}

	internal->exit = true;

	/* Execute the current states exit action */
	if (ctx->current->exit) {
		ctx->current->exit(ctx);

		/*
		 * No need to continue if terminate was set in the
		 * exit action
		 */
		if (internal->terminate) {
			return;
		}
	}

	if (IS_ENABLED(CONFIG_SMF_ANCESTOR_SUPPORT)) {
		internal->new_state = true;

		if (smf_execute_ancestor_exit_actions(ctx, target)) {
			return;
		}
	}

	internal->exit = false;

	/* update the state variables */
	ctx->previous = ctx->current;
	ctx->current = target;

	if (IS_ENABLED(CONFIG_SMF_ANCESTOR_SUPPORT)) {
		if (smf_execute_ancestor_entry_actions(ctx, target)) {
			return;
		}
	}

	/* Now execute the target entry action */
	if (ctx->current->entry) {
		ctx->current->entry(ctx);
		/*
		 * If terminate was set, it will be handled in the
		 * smf_run_state function
		 */
	}
}

void smf_set_terminate(struct smf_ctx *ctx, int32_t val)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;

	internal->terminate = true;
	ctx->terminate_val = val;
}


int32_t smf_run_state(struct smf_ctx *const ctx)
{
	struct internal_ctx * const internal = (void *) &ctx->internal;

	/* No need to continue if terminate was set */
	if (internal->terminate) {
		return ctx->terminate_val;
	}

	if (ctx->current->run) {
		ctx->current->run(ctx);
	}

	if (IS_ENABLED(CONFIG_SMF_ANCESTOR_SUPPORT)) {
		if (smf_execute_ancestor_run_actions(ctx)) {
			return ctx->terminate_val;
		}
	}

	return 0;
}
