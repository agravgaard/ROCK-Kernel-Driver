/* SPDX-License-Identifier: MIT */
#ifndef AMDKCL_KERNEL_H
#define AMDKCL_KERNEL_H

#ifndef u64_to_user_ptr
#define u64_to_user_ptr(x) (	\
{					\
	typecheck(u64, x);		\
	(void __user *)(uintptr_t)x;	\
}					\
)
#endif

#ifndef __GFP_RETRY_MAYFAIL
#define __GFP_RETRY_MAYFAIL __GFP_REPEAT
#endif

#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, a)	__ALIGN_KERNEL((x) - ((a) - 1), (a))
#endif /* ALIGN_DOWN */

#endif /* AMDKCL_KERNEL_H */
