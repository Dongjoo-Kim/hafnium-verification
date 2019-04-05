/*
 * Copyright 2019 The Hafnium Authors.
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

#include "hf/std.h"

#include "hf/panic.h"

/* Declare unsafe functions locally so they are not available globally. */
void *memset(void *s, int c, size_t n);

void memset_s(void *dest, rsize_t destsz, int ch, rsize_t count)
{
	if (dest == NULL) {
		goto fail;
	}

	if (destsz > RSIZE_MAX || count > RSIZE_MAX) {
		goto fail;
	}

	if (count > destsz) {
		goto fail;
	}

	memset(dest, ch, count);
	return;

fail:
	panic("memset_s failure");
}