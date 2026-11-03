// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 ishad0w and the poulsback contributors

#include "poulsback_bo_use.h"

sgx535_u32 poulsback_bo_use_close(struct poulsback_bo_use *use)
{
	return !--use->handles && !use->jobs && !use->kept;
}

sgx535_u32 poulsback_bo_use_release(struct poulsback_bo_use *use)
{
	return !--use->jobs && !use->handles && !use->kept;
}
