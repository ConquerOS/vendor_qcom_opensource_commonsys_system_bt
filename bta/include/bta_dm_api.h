/******************************************************************************
 *
 *  Copyright (C) 2006-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This is the interface file for device mananger functions.
 *
 ******************************************************************************/
#ifndef BTA_DM_API_H
#define BTA_DM_API_H

#include "stack/include/bt_types.h"
#include "bta/dm/bta_dm_int.h"

// Brings connection to active mode
void bta_dm_pm_active(const RawAddress& peer_addr);
void bta_dm_gatt_le_services(RawAddress bd_addr);

#endif /* BTA_DM_API_H */
