/*
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This file is a part of the CANN Open Software.
 * Licensed under CANN Open Software License Agreement Version 1.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#ifndef ACT_DETAIL_MACROS_HPP
#define ACT_DETAIL_MACROS_HPP

#define ACT_DEVICE __forceinline__ [aicore]
#define ACT_HOST_DEVICE __forceinline__ [host, aicore]
#define ACT_GLOBAL __global__ [aicore]

#endif  // ACT_DETAIL_MACROS_HPP