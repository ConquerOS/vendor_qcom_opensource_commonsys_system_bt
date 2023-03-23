/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
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
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *  Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *  SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains functions for BLE device control utilities, and LE
 *  security functions.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btm_ble"

#include "bt_target.h"

#include <base/bind.h>
#include <string.h>

#include "bt_types.h"
#include "bt_utils.h"
#include "btm_ble_api.h"
#include "btm_int.h"
#include "btu.h"
#include "device/include/controller.h"
#include "gap_api.h"
#include "gatt_api.h"
#include "hcimsgs.h"
#include "log/log.h"
#include "l2c_int.h"
#include "osi/include/log.h"
#include "osi/include/osi.h"
#include "osi/include/properties.h"
#include "stack/crypto_toolbox/crypto_toolbox.h"
#include "btif/include/btif_storage.h"
#include "stack_config.h"
#include "stack/gatt/connection_manager.h"
#include "btif/include/btif_config.h"

extern void gatt_notify_phy_updated(uint8_t status, uint16_t handle,
                                    uint8_t tx_phy, uint8_t rx_phy);
extern void btm_send_link_key_notif(tBTM_SEC_DEV_REC* p_dev_rec);

#ifdef ADV_AUDIO_FEATURE
extern bool is_remote_support_adv_audio(const RawAddress remote_bdaddr);
#endif

//HCI Command or Event callbacks
tBTM_BLE_HCI_CMD_CB hci_cmd_cmpl;
// Key: cis_handle, Value: cig_id
std::map<uint16_t, uint8_t> cis_map;
// pending CIS connection
std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN> pending_cis_map;
uint16_t last_pending_cis_handle = 0;
/******************************************************************************/
/* External Function to be called by other modules                            */
/******************************************************************************/
/********************************************************
 *
 * Function         BTM_SecAddBleDevice
 *
 * Description      Add/modify device.  This function will be normally called
 *                  during host startup to restore all required information
 *                  for a LE device stored in the NVRAM.
 *
 * Parameters:      bd_addr          - BD address of the peer
 *                  bd_name          - Name of the peer device. NULL if unknown.
 *                  dev_type         - Remote device's device type.
 *                  addr_type        - LE device address type.
 *
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool BTM_SecAddBleDevice(const RawAddress& bd_addr, BD_NAME bd_name,
                         tBT_DEVICE_TYPE dev_type, tBLE_ADDR_TYPE addr_type) {
  BTM_TRACE_DEBUG("%s: dev_type=0x%x", __func__, dev_type);

  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  if (!p_dev_rec) {
    p_dev_rec = btm_sec_alloc_dev(bd_addr);

    BTM_TRACE_DEBUG("%s: Device added, handle=0x%x, p_dev_rec=%p, bd_addr=%s",
                    __func__, p_dev_rec->ble_hci_handle, p_dev_rec,
                    bd_addr.ToString().c_str());
  }

  memset(p_dev_rec->sec_bd_name, 0, sizeof(tBTM_BD_NAME));

  if (bd_name && bd_name[0]) {
    p_dev_rec->sec_flags |= BTM_SEC_NAME_KNOWN;
    strlcpy((char*)p_dev_rec->sec_bd_name, (char*)bd_name,
            BTM_MAX_REM_BD_NAME_LEN + 1);
  }
  p_dev_rec->device_type |= dev_type;
  p_dev_rec->ble.ble_addr_type = addr_type;

  p_dev_rec->ble.pseudo_addr = bd_addr;
  /* sync up with the Inq Data base*/
  tBTM_INQ_INFO* p_info = BTM_InqDbRead(bd_addr);
  if (p_info) {
    p_info->results.ble_addr_type = p_dev_rec->ble.ble_addr_type;
    p_info->results.device_type = p_dev_rec->device_type;
    BTM_TRACE_DEBUG("InqDb  device_type =0x%x  addr_type=0x%x",
                    p_info->results.device_type, p_info->results.ble_addr_type);
  }

  return true;
}
/*******************************************************************************
 *
 * Function         BTM_GetRemoteDeviceName
 *
 * Description      This function is called to get the dev name of remote device
 *                  from NV
 *
 * Returns          TRUE if success; otherwise failed.
 *
 ******************************************************************************/
bool BTM_GetRemoteDeviceName(const RawAddress& bd_addr, BD_NAME bd_name)
{
  BTM_TRACE_DEBUG("%s", __func__);
  bool ret = FALSE;
  bt_bdname_t bdname;
  bt_property_t prop_name;
  BTIF_STORAGE_FILL_PROPERTY(&prop_name, BT_PROPERTY_BDNAME,
                            sizeof(bt_bdname_t), &bdname);

  if (btif_storage_get_remote_device_property(
      &bd_addr, &prop_name) == BT_STATUS_SUCCESS) {
    APPL_TRACE_DEBUG("%s, NV name = %s", __func__, bdname.name);
    strlcpy((char*) bd_name, (char*) bdname.name, BD_NAME_LEN + 1);
    ret = TRUE;
  }
  return ret;
}
/*******************************************************************************
 *
 * Function         BTM_SecAddBleKey
 *
 * Description      Add/modify LE device information.  This function will be
 *                  normally called during host startup to restore all required
 *                  information stored in the NVRAM.
 *
 * Parameters:      bd_addr          - BD address of the peer
 *                  p_le_key         - LE key values.
 *                  key_type         - LE SMP key type.
*
 * Returns          true if added OK, else false
 *
 ******************************************************************************/
bool BTM_SecAddBleKey(const RawAddress& bd_addr, tBTM_LE_KEY_VALUE* p_le_key,
                      tBTM_LE_KEY_TYPE key_type) {
  tBTM_SEC_DEV_REC* p_dev_rec;
  BTM_TRACE_DEBUG("BTM_SecAddBleKey");
  p_dev_rec = btm_find_dev(bd_addr);
  if (!p_dev_rec || !p_le_key ||
      (key_type != BTM_LE_KEY_PENC && key_type != BTM_LE_KEY_PID &&
       key_type != BTM_LE_KEY_PCSRK && key_type != BTM_LE_KEY_LENC &&
       key_type != BTM_LE_KEY_LCSRK && key_type != BTM_LE_KEY_LID)) {
    LOG(WARNING) << __func__
                 << " Wrong Type, or No Device record for bdaddr: " << bd_addr
                 << ", Type: " << key_type;
    return (false);
  }

  VLOG(1) << __func__ << " BDA: " << bd_addr << ", Type: " << key_type;

  btm_sec_save_le_key(bd_addr, key_type, p_le_key, false);

#if (BLE_PRIVACY_SPT == TRUE)
  if (key_type == BTM_LE_KEY_PID || key_type == BTM_LE_KEY_LID)
    btm_ble_resolving_list_load_dev(p_dev_rec);
#endif

  return (true);
}

/*******************************************************************************
 *
 * Function         BTM_BleLoadLocalKeys
 *
 * Description      Local local identity key, encryption root or sign counter.
 *
 * Parameters:      key_type: type of key, can be BTM_BLE_KEY_TYPE_ID,
 *                                                BTM_BLE_KEY_TYPE_ER
 *                                             or BTM_BLE_KEY_TYPE_COUNTER.
 *                  p_key: pointer to the key.
 *
 * Returns          non2.
 *
 ******************************************************************************/
void BTM_BleLoadLocalKeys(uint8_t key_type, tBTM_BLE_LOCAL_KEYS* p_key) {
  tBTM_DEVCB* p_devcb = &btm_cb.devcb;
  BTM_TRACE_DEBUG("%s", __func__);
  if (p_key != NULL) {
    switch (key_type) {
      case BTM_BLE_KEY_TYPE_ID:
        memcpy(&p_devcb->id_keys, &p_key->id_keys,
               sizeof(tBTM_BLE_LOCAL_ID_KEYS));
        break;

      case BTM_BLE_KEY_TYPE_ER:
        p_devcb->ble_encryption_key_value = p_key->er;
        break;

      default:
        BTM_TRACE_ERROR("unknow local key type: %d", key_type);
        break;
    }
  }
}

/** Returns local device encryption root (ER) */
const Octet16& BTM_GetDeviceEncRoot() {
  return btm_cb.devcb.ble_encryption_key_value;
}

/** Returns local device identity root (IR). */
const Octet16& BTM_GetDeviceIDRoot() { return btm_cb.devcb.id_keys.irk; }

/** Return local device DHK. */
const Octet16& BTM_GetDeviceDHK() { return btm_cb.devcb.id_keys.dhk; }

/*******************************************************************************
 *
 * Function         BTM_ReadConnectionAddr
 *
 * Description      This function is called to get the local device address
 *                  information.
 *
 * Returns          void
 *
 ******************************************************************************/
void BTM_ReadConnectionAddr(const RawAddress& remote_bda,
                            RawAddress& local_conn_addr,
                            tBLE_ADDR_TYPE* p_addr_type) {
  tACL_CONN* p_acl = btm_bda_to_acl(remote_bda, BT_TRANSPORT_LE);

  if (p_acl == NULL) {
    BTM_TRACE_ERROR("No connection exist!");
    return;
  }
  local_conn_addr = p_acl->conn_addr;
  *p_addr_type = p_acl->conn_addr_type;

  BTM_TRACE_DEBUG("BTM_ReadConnectionAddr address type: %d addr: 0x%02x",
                  p_acl->conn_addr_type, p_acl->conn_addr.address[0]);
}

/*******************************************************************************
 *
 * Function         BTM_IsBleConnection
 *
 * Description      This function is called to check if the connection handle
 *                  for an LE link
 *
 * Returns          true if connection is LE link, otherwise false.
 *
 ******************************************************************************/
bool BTM_IsBleConnection(uint16_t conn_handle) {
  uint8_t xx;
  tACL_CONN* p;

  BTM_TRACE_API("BTM_IsBleConnection: conn_handle: %d", conn_handle);

  xx = btm_handle_to_acl_index(conn_handle);
  if (xx >= MAX_L2CAP_LINKS) return false;

  p = &btm_cb.acl_db[xx];

  return (p->transport == BT_TRANSPORT_LE);
}

/*******************************************************************************
 *
 * Function       BTM_ReadRemoteConnectionAddr
 *
 * Description    This function is read the remote device address currently used
 *
 * Parameters     pseudo_addr: pseudo random address available
 *                conn_addr:connection address used
 *                p_addr_type : BD Address type, Public or Random of the address
 *                              used
 *
 * Returns        bool, true if connection to remote device exists, else false
 *
 ******************************************************************************/
bool BTM_ReadRemoteConnectionAddr(const RawAddress& pseudo_addr,
                                  RawAddress& conn_addr,
                                  tBLE_ADDR_TYPE* p_addr_type) {
  bool st = true;
#if (BLE_PRIVACY_SPT == TRUE)
  tACL_CONN* p = btm_bda_to_acl(pseudo_addr, BT_TRANSPORT_LE);

  if (p == NULL) {
    BTM_TRACE_ERROR(
        "BTM_ReadRemoteConnectionAddr can not find connection"
        " with matching address");
    return false;
  }

  conn_addr = p->active_remote_addr;
  *p_addr_type = p->active_remote_addr_type;
#else
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(pseudo_addr);

  conn_addr = pseudo_addr;
  if (p_dev_rec != NULL) {
    *p_addr_type = p_dev_rec->ble.ble_addr_type;
  }
#endif
  return st;
}
/*******************************************************************************
 *
 * Function         BTM_SecurityGrant
 *
 * Description      This function is called to grant security process.
 *
 * Parameters       bd_addr - peer device bd address.
 *                  res     - result of the operation BTM_SUCCESS if success.
 *                            Otherwise, BTM_REPEATED_ATTEMPTS if too many
 *                            attempts.
 *
 * Returns          None
 *
 ******************************************************************************/
void BTM_SecurityGrant(const RawAddress& bd_addr, uint8_t res) {
  tSMP_STATUS res_smp =
      (res == BTM_SUCCESS) ? SMP_SUCCESS : SMP_REPEATED_ATTEMPTS;
  BTM_TRACE_DEBUG("BTM_SecurityGrant");
  SMP_SecurityGrant(bd_addr, res_smp);
}

/*******************************************************************************
 *
 * Function         BTM_BlePasskeyReply
 *
 * Description      This function is called after Security Manager submitted
 *                  passkey request to the application.
 *
 * Parameters:      bd_addr - Address of the device for which passkey was
 *                            requested
 *                  res     - result of the operation BTM_SUCCESS if success
 *                  key_len - length in bytes of the Passkey
 *                  p_passkey    - pointer to array with the passkey
 *                  trusted_mask - bitwise OR of trusted services (array of
 *                                 uint32_t)
 *
 ******************************************************************************/
void BTM_BlePasskeyReply(const RawAddress& bd_addr, uint8_t res,
                         uint32_t passkey) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  tSMP_STATUS res_smp =
      (res == BTM_SUCCESS) ? SMP_SUCCESS : SMP_PASSKEY_ENTRY_FAIL;

  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("Passkey reply to Unknown device");
    return;
  }

  p_dev_rec->sec_flags |= BTM_SEC_LE_AUTHENTICATED;
  BTM_TRACE_DEBUG("BTM_BlePasskeyReply");
  SMP_PasskeyReply(bd_addr, res_smp, passkey);
}

/*******************************************************************************
 *
 * Function         BTM_BleConfirmReply
 *
 * Description      This function is called after Security Manager submitted
 *                  numeric comparison request to the application.
 *
 * Parameters:      bd_addr      - Address of the device with which numeric
 *                                 comparison was requested
 *                  res          - comparison result BTM_SUCCESS if success
 *
 ******************************************************************************/
void BTM_BleConfirmReply(const RawAddress& bd_addr, uint8_t res) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  tSMP_STATUS res_smp =
      (res == BTM_SUCCESS) ? SMP_SUCCESS : SMP_PASSKEY_ENTRY_FAIL;

  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("Passkey reply to Unknown device");
    return;
  }

  p_dev_rec->sec_flags |= BTM_SEC_LE_AUTHENTICATED;
  BTM_TRACE_DEBUG("%s", __func__);
  SMP_ConfirmReply(bd_addr, res_smp);
}

/*******************************************************************************
 *
 * Function         BTM_BleOobDataReply
 *
 * Description      This function is called to provide the OOB data for
 *                  SMP in response to BTM_LE_OOB_REQ_EVT
 *
 * Parameters:      bd_addr     - Address of the peer device
 *                  res         - result of the operation SMP_SUCCESS if success
 *                  p_data      - oob data, depending on transport and
 *                                capabilities.
 *                                Might be "Simple Pairing Randomizer", or
 *                                "Security Manager TK Value".
 *
 ******************************************************************************/
void BTM_BleOobDataReply(const RawAddress& bd_addr, uint8_t res, uint8_t len,
                         uint8_t* p_data) {
  tSMP_STATUS res_smp = (res == BTM_SUCCESS) ? SMP_SUCCESS : SMP_OOB_FAIL;
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);

  BTM_TRACE_DEBUG("%s:", __func__);

  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("%s: Unknown device", __func__);
    return;
  }

  p_dev_rec->sec_flags |= BTM_SEC_LE_AUTHENTICATED;
  SMP_OobDataReply(bd_addr, res_smp, len, p_data);
}

/*******************************************************************************
 *
 * Function         BTM_BleSecureConnectionOobDataReply
 *
 * Description      This function is called to provide the OOB data for
 *                  SMP in response to BTM_LE_OOB_REQ_EVT when secure connection
 *                  data is available
 *
 * Parameters:      bd_addr     - Address of the peer device
 *                  p_c         - pointer to Confirmation.
 *                  p_r         - pointer to Randomizer
 *
 ******************************************************************************/
void BTM_BleSecureConnectionOobDataReply(const RawAddress& bd_addr,
                                         uint8_t* p_c, uint8_t* p_r) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);

  BTM_TRACE_DEBUG("%s:", __func__);

  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("%s: Unknown device", __func__);
    return;
  }

  p_dev_rec->sec_flags |= BTM_SEC_LE_AUTHENTICATED;

  tSMP_SC_OOB_DATA oob;
  memset(&oob, 0, sizeof(tSMP_SC_OOB_DATA));

  oob.peer_oob_data.present = true;
  memcpy(&oob.peer_oob_data.randomizer, p_r, OCTET16_LEN);
  memcpy(&oob.peer_oob_data.commitment, p_c, OCTET16_LEN);
  oob.peer_oob_data.addr_rcvd_from.type = p_dev_rec->ble.ble_addr_type;
  oob.peer_oob_data.addr_rcvd_from.bda = bd_addr;

  SMP_SecureConnectionOobDataReply((uint8_t*)&oob);
}

/******************************************************************************
 *
 * Function         BTM_BleSetConnScanParams
 *
 * Description      Set scan parameter used in BLE connection request
 *
 * Parameters:      scan_interval: scan interval
 *                  scan_window: scan window
 *
 * Returns          void
 *
 ******************************************************************************/
void BTM_BleSetConnScanParams(uint32_t scan_interval, uint32_t scan_window) {
  tBTM_BLE_CB* p_ble_cb = &btm_cb.ble_ctr_cb;
  bool new_param = false;

  if (BTM_BLE_ISVALID_PARAM(scan_interval, BTM_BLE_SCAN_INT_MIN,
                            BTM_BLE_SCAN_INT_MAX) &&
      BTM_BLE_ISVALID_PARAM(scan_window, BTM_BLE_SCAN_WIN_MIN,
                            BTM_BLE_SCAN_WIN_MAX)) {
    if (p_ble_cb->scan_int != scan_interval) {
      p_ble_cb->scan_int = scan_interval;
      new_param = true;
    }

    if (p_ble_cb->scan_win != scan_window) {
      p_ble_cb->scan_win = scan_window;
      new_param = true;
    }

    if (new_param && btm_ble_get_conn_st() == BLE_CONNECTING) {
      btm_ble_suspend_bg_conn();
    }
  } else {
    BTM_TRACE_ERROR("Illegal Connection Scan Parameters");
  }
}

/********************************************************
 *
 * Function         BTM_BleSetPrefConnParams
 *
 * Description      Set a peripheral's preferred connection parameters
 *
 * Parameters:      bd_addr          - BD address of the peripheral
 *                  scan_interval: scan interval
 *                  scan_window: scan window
 *                  min_conn_int     - minimum preferred connection interval
 *                  max_conn_int     - maximum preferred connection interval
 *                  slave_latency    - preferred slave latency
 *                  supervision_tout - preferred supervision timeout
 *
 * Returns          void
 *
 ******************************************************************************/
void BTM_BleSetPrefConnParams(const RawAddress& bd_addr, uint16_t min_conn_int,
                              uint16_t max_conn_int, uint16_t slave_latency,
                              uint16_t supervision_tout) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);

  BTM_TRACE_API(
      "BTM_BleSetPrefConnParams min: %u  max: %u  latency: %u  \
                    tout: %u",
      min_conn_int, max_conn_int, slave_latency, supervision_tout);

  if (BTM_BLE_ISVALID_PARAM(min_conn_int, BTM_BLE_CONN_INT_MIN,
                            BTM_BLE_CONN_INT_MAX) &&
      BTM_BLE_ISVALID_PARAM(max_conn_int, BTM_BLE_CONN_INT_MIN,
                            BTM_BLE_CONN_INT_MAX) &&
      BTM_BLE_ISVALID_PARAM(supervision_tout, BTM_BLE_CONN_SUP_TOUT_MIN,
                            BTM_BLE_CONN_SUP_TOUT_MAX) &&
      (slave_latency <= BTM_BLE_CONN_LATENCY_MAX ||
       slave_latency == BTM_BLE_CONN_PARAM_UNDEF)) {
    if (p_dev_rec) {
      /* expect conn int and stout and slave latency to be updated all together
       */
      if (min_conn_int != BTM_BLE_CONN_PARAM_UNDEF ||
          max_conn_int != BTM_BLE_CONN_PARAM_UNDEF) {
        if (min_conn_int != BTM_BLE_CONN_PARAM_UNDEF)
          p_dev_rec->conn_params.min_conn_int = min_conn_int;
        else
          p_dev_rec->conn_params.min_conn_int = max_conn_int;

        if (max_conn_int != BTM_BLE_CONN_PARAM_UNDEF)
          p_dev_rec->conn_params.max_conn_int = max_conn_int;
        else
          p_dev_rec->conn_params.max_conn_int = min_conn_int;

        if (slave_latency != BTM_BLE_CONN_PARAM_UNDEF)
          p_dev_rec->conn_params.slave_latency = slave_latency;
        else
          p_dev_rec->conn_params.slave_latency = BTM_BLE_CONN_SLAVE_LATENCY_DEF;

        if (supervision_tout != BTM_BLE_CONN_PARAM_UNDEF)
          p_dev_rec->conn_params.supervision_tout = supervision_tout;
        else
          p_dev_rec->conn_params.supervision_tout = BTM_BLE_CONN_TIMEOUT_DEF;
      }

    } else {
      BTM_TRACE_ERROR("Unknown Device, setting rejected");
    }
  } else {
    BTM_TRACE_ERROR("Illegal Connection Parameters");
  }
}

/*******************************************************************************
 *
 * Function         BTM_ReadDevInfo
 *
 * Description      This function is called to read the device/address type
 *                  of BD address.
 *
 * Parameter        remote_bda: remote device address
 *                  p_dev_type: output parameter to read the device type.
 *                  p_addr_type: output parameter to read the address type.
 *
 ******************************************************************************/
void BTM_ReadDevInfo(const RawAddress& remote_bda, tBT_DEVICE_TYPE* p_dev_type,
                     tBLE_ADDR_TYPE* p_addr_type) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(remote_bda);
  tBTM_INQ_INFO* p_inq_info = BTM_InqDbRead(remote_bda);

  *p_addr_type = BLE_ADDR_PUBLIC;

  if (!p_dev_rec) {
    *p_dev_type = BT_DEVICE_TYPE_BREDR;
    /* Check with the BT manager if details about remote device are known */
    if (p_inq_info != NULL) {
      *p_dev_type = p_inq_info->results.device_type;
      *p_addr_type = p_inq_info->results.ble_addr_type;
    } else {
      /* unknown device, assume BR/EDR */
      BTM_TRACE_DEBUG("btm_find_dev_type - unknown device, BR/EDR assumed");
    }
  } else /* there is a security device record exisitng */
  {
    /* new inquiry result, overwrite device type in security device record */
    if (p_inq_info) {
      BTM_TRACE_DEBUG("p_dev_rec->device_type -%d",p_dev_rec->device_type);
      p_dev_rec->device_type |= p_inq_info->results.device_type;
      p_dev_rec->ble.ble_addr_type = p_inq_info->results.ble_addr_type;
    }
    if (p_dev_rec->bd_addr == remote_bda &&
        p_dev_rec->ble.pseudo_addr == remote_bda) {
      *p_dev_type = p_dev_rec->device_type;
      *p_addr_type = p_dev_rec->ble.ble_addr_type;
    } else if (p_dev_rec->ble.pseudo_addr == remote_bda) {
      *p_dev_type = BT_DEVICE_TYPE_BLE;
      *p_addr_type = p_dev_rec->ble.ble_addr_type;
    } else /* matching static adddress only */
    {
      *p_dev_type = BT_DEVICE_TYPE_BREDR;
      *p_addr_type = BLE_ADDR_PUBLIC;
    }
  }

  BTM_TRACE_DEBUG("btm_find_dev_type - device_type = %d addr_type = %d",
                  *p_dev_type, *p_addr_type);
}

/*******************************************************************************
 *
 * Function         BTM_ReadDevScanInfo
 *
 * Description      This function is called to read the device/address type
 *                  of BD address.
 *
 * Parameter        remote_bda: remote device address
 *                  p_dev_type: output parameter to read the device type.
 *                  p_addr_type: output parameter to read the address type.
 *
 ******************************************************************************/
void BTM_ReadDevScanInfo(const RawAddress& remote_bda, tBT_DEVICE_TYPE* p_dev_type,
                     tBLE_ADDR_TYPE* p_addr_type) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(remote_bda);
  tBTM_INQ_INFO* p_inq_info = BTM_InqDbRead(remote_bda);

  *p_addr_type = BLE_ADDR_PUBLIC_ID;

  if (!p_dev_rec) {
    *p_dev_type = BT_DEVICE_TYPE_BREDR;
    /* Check with the BT manager if details about remote device are known */
    if (p_inq_info != NULL) {
      *p_dev_type = p_inq_info->results.device_type;
      *p_addr_type = p_inq_info->results.ble_addr_type;
    } else {
      /* unknown device, assume BR/EDR */
      BTM_TRACE_DEBUG("btm_find_dev_type - unknown device, BR/EDR assumed");
    }
  } else /* there is a security device record exisitng */
  {
    /* new inquiry result, overwrite device type in security device record */
    if (p_inq_info) {
      BTM_TRACE_DEBUG("p_dev_rec->device_type -%d",p_dev_rec->device_type);
      p_dev_rec->device_type |= p_inq_info->results.device_type;
      p_dev_rec->ble.ble_addr_type = p_inq_info->results.ble_addr_type;
    }
    if (p_dev_rec->bd_addr == remote_bda &&
        p_dev_rec->ble.pseudo_addr == remote_bda) {
      *p_dev_type = p_dev_rec->device_type;
      *p_addr_type = p_dev_rec->ble.ble_addr_type;
    } else if (p_dev_rec->ble.pseudo_addr == remote_bda) {
      *p_dev_type = BT_DEVICE_TYPE_BLE;
      *p_addr_type = p_dev_rec->ble.ble_addr_type;
    } else /* matching static adddress only */
    {
      *p_dev_type = BT_DEVICE_TYPE_BREDR;
      *p_addr_type = BLE_ADDR_PUBLIC;
    }
  }

  BTM_TRACE_DEBUG("btm_find_dev_type - device_type = %d addr_type = %d",
                  *p_dev_type, *p_addr_type);
}


/*******************************************************************************
 *
 * Function         BTM_ReadConnectedTransportAddress
 *
 * Description      This function is called to read the paired device/address
 *                  type of other device paired corresponding to the BD_address
 *
 * Parameter        remote_bda: remote device address, carry out the transport
 *                              address
 *                  transport: active transport
 *
 * Return           true if an active link is identified; false otherwise
 *
 ******************************************************************************/
bool BTM_ReadConnectedTransportAddress(RawAddress* remote_bda,
                                       tBT_TRANSPORT transport) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(*remote_bda);

  /* if no device can be located, return */
  if (p_dev_rec == NULL) return false;

  if (transport == BT_TRANSPORT_BR_EDR) {
    if (btm_bda_to_acl(p_dev_rec->bd_addr, transport) != NULL) {
      *remote_bda = p_dev_rec->bd_addr;
      return true;
    } else if (p_dev_rec->device_type & BT_DEVICE_TYPE_BREDR) {
      *remote_bda = p_dev_rec->bd_addr;
    } else
      *remote_bda = RawAddress::kEmpty;
    return false;
  } else if (transport == BT_TRANSPORT_LE) {
    *remote_bda = p_dev_rec->ble.pseudo_addr;
    if (btm_bda_to_acl(p_dev_rec->ble.pseudo_addr, transport) != NULL)
      return true;
    else
      return false;
  } else {
    //INVALID transport , finding other device that doesnt match the address
    if(*remote_bda != p_dev_rec->bd_addr)
    {
      *remote_bda = p_dev_rec->bd_addr;
      if (btm_bda_to_acl(p_dev_rec->bd_addr, BT_TRANSPORT_BR_EDR) != NULL)
        return true;
      else
        return false;
    } else if (*remote_bda != p_dev_rec->ble.pseudo_addr) {
      *remote_bda = p_dev_rec->ble.pseudo_addr;
      if (btm_bda_to_acl(p_dev_rec->ble.pseudo_addr, BT_TRANSPORT_LE) != NULL)
        return true;
      else
        return false;
    } else {
      memset(remote_bda, 0, BD_ADDR_LEN);
      return false;
    }
  }

  return false;
}

/*******************************************************************************
 *
 * Function         BTM_BleReceiverTest
 *
 * Description      This function is called to start the LE Receiver test
 *
 * Parameter       rx_freq - Frequency Range
 *               p_cmd_cmpl_cback - Command Complete callback
 *
 ******************************************************************************/
void BTM_BleReceiverTest(uint8_t rx_freq, tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  btm_cb.devcb.p_le_test_cmd_cmpl_cb = p_cmd_cmpl_cback;

  btsnd_hcic_ble_receiver_test(rx_freq);
}

/*******************************************************************************
 *
 * Function         BTM_BleTransmitterTest
 *
 * Description      This function is called to start the LE Transmitter test
 *
 * Parameter       tx_freq - Frequency Range
 *                       test_data_len - Length in bytes of payload data in each
 *                                       packet
 *                       packet_payload - Pattern to use in the payload
 *                       p_cmd_cmpl_cback - Command Complete callback
 *
 ******************************************************************************/
void BTM_BleTransmitterTest(uint8_t tx_freq, uint8_t test_data_len,
                            uint8_t packet_payload,
                            tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  btm_cb.devcb.p_le_test_cmd_cmpl_cb = p_cmd_cmpl_cback;
  btsnd_hcic_ble_transmitter_test(tx_freq, test_data_len, packet_payload);
}

/*******************************************************************************
 *
 * Function         BTM_BleTestEnd
 *
 * Description      This function is called to stop the in-progress TX or RX
 *                  test
 *
 * Parameter       p_cmd_cmpl_cback - Command complete callback
 *
 ******************************************************************************/
void BTM_BleTestEnd(tBTM_CMPL_CB* p_cmd_cmpl_cback) {
  btm_cb.devcb.p_le_test_cmd_cmpl_cb = p_cmd_cmpl_cback;

  btsnd_hcic_ble_test_end();
}

/*******************************************************************************
 * Internal Functions
 ******************************************************************************/
void btm_ble_test_command_complete(uint8_t* p) {
  tBTM_CMPL_CB* p_cb = btm_cb.devcb.p_le_test_cmd_cmpl_cb;

  btm_cb.devcb.p_le_test_cmd_cmpl_cb = NULL;

  if (p_cb) {
    (*p_cb)(p);
  }
}

/*******************************************************************************
 *
 * Function         BTM_UseLeLink
 *
 * Description      This function is to select the underlying physical link to
 *                  use.
 *
 * Returns          true to use LE, false use BR/EDR.
 *
 ******************************************************************************/
bool BTM_UseLeLink(const RawAddress& bd_addr) {
  tACL_CONN* p;
  tBT_DEVICE_TYPE dev_type;
  tBLE_ADDR_TYPE addr_type;
  bool use_le = false;

  p = btm_bda_to_acl(bd_addr, BT_TRANSPORT_BR_EDR);
  if (p != NULL) {
    return use_le;
  } else {
    p = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);
    if (p != NULL) {
      use_le = true;
    } else {
      BTM_ReadDevInfo(bd_addr, &dev_type, &addr_type);
      use_le = (dev_type == BT_DEVICE_TYPE_BLE);
    }
  }
  return use_le;
}

/*******************************************************************************
 *
 * Function         BTM_SetBleDataLength
 *
 * Description      This function is to set maximum BLE transmission packet size
 *
 * Returns          BTM_SUCCESS if success; otherwise failed.
 *
 ******************************************************************************/
tBTM_STATUS BTM_SetBleDataLength(const RawAddress& bd_addr,
                                 uint16_t tx_pdu_length) {
  tACL_CONN* p_acl = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);
  uint16_t tx_time = BTM_BLE_DATA_TX_TIME_MAX_LEGACY;

  if (p_acl == NULL) {
    BTM_TRACE_ERROR("%s: Wrong mode: no LE link exist or LE not supported",
                    __func__);
    return BTM_WRONG_MODE;
  }

  BTM_TRACE_DEBUG("%s: tx_pdu_length =%d", __func__, tx_pdu_length);

  if (!controller_get_interface()->supports_ble_packet_extension()) {
    BTM_TRACE_ERROR("%s failed, request not supported", __func__);
    return BTM_ILLEGAL_VALUE;
  }

  if (!HCI_LE_DATA_LEN_EXT_SUPPORTED(p_acl->peer_le_features)) {
    BTM_TRACE_ERROR("%s failed, peer does not support request", __func__);
    return BTM_ILLEGAL_VALUE;
  }

  if (tx_pdu_length > BTM_BLE_DATA_SIZE_MAX)
    tx_pdu_length = BTM_BLE_DATA_SIZE_MAX;
  else if (tx_pdu_length < BTM_BLE_DATA_SIZE_MIN)
    tx_pdu_length = BTM_BLE_DATA_SIZE_MIN;

  if (controller_get_interface()->get_bt_version()->hci_version >= HCI_PROTO_VERSION_5_0)
    tx_time = BTM_BLE_DATA_TX_TIME_MAX;

  /* always set the TxTime to be max, as controller does not care for now */
  btsnd_hcic_ble_set_data_length(p_acl->hci_handle, tx_pdu_length, tx_time);

  return BTM_SUCCESS;
}

void read_phy_cb(
    base::Callback<void(uint8_t tx_phy, uint8_t rx_phy, uint8_t status)> cb,
    uint8_t* data, uint16_t len) {
  uint8_t status, tx_phy, rx_phy;
  uint16_t handle;

  LOG_ASSERT(len == 5) << "Received bad response length: " << len;
  uint8_t* pp = data;
  STREAM_TO_UINT8(status, pp);
  STREAM_TO_UINT16(handle, pp);
  handle = handle & 0x0FFF;
  STREAM_TO_UINT8(tx_phy, pp);
  STREAM_TO_UINT8(rx_phy, pp);

  DVLOG(1) << __func__ << " Received read_phy_cb";
  cb.Run(tx_phy, rx_phy, status);
}

/*******************************************************************************
 *
 * Function         BTM_BleReadPhy
 *
 * Description      To read the current PHYs for specified LE connection
 *
 *
 * Returns          BTM_SUCCESS if command successfully sent to controller,
 *                  BTM_MODE_UNSUPPORTED if local controller doesn't support LE
 *                  2M or LE Coded PHY,
 *                  BTM_WRONG_MODE if Device in wrong mode for request.
 *
 ******************************************************************************/
void BTM_BleReadPhy(
    const RawAddress& bd_addr,
    base::Callback<void(uint8_t tx_phy, uint8_t rx_phy, uint8_t status)> cb) {
  BTM_TRACE_DEBUG("%s", __func__);

  tACL_CONN* p_acl = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);

  if (p_acl == NULL) {
    BTM_TRACE_ERROR("%s: Wrong mode: no LE link exist or LE not supported",
                    __func__);
    cb.Run(0, 0, HCI_ERR_NO_CONNECTION);
    return;
  }

  // checking if local controller supports it!
  if (!controller_get_interface()->supports_ble_2m_phy() &&
      !controller_get_interface()->supports_ble_coded_phy()) {
    BTM_TRACE_ERROR("%s failed, request not supported in local controller!",
                    __func__);
    cb.Run(0, 0, GATT_REQ_NOT_SUPPORTED);
    return;
  }

  uint16_t handle = p_acl->hci_handle;

  const uint8_t len = HCIC_PARAM_SIZE_BLE_READ_PHY;
  uint8_t data[len];
  uint8_t* pp = data;
  UINT16_TO_STREAM(pp, handle);
  btu_hcif_send_cmd_with_cb(FROM_HERE, HCI_BLE_READ_PHY, data, len,
                            base::Bind(&read_phy_cb, std::move(cb)));
  return;
}

void doNothing(uint8_t* data, uint16_t len) {}

/*******************************************************************************
 *
 * Function         BTM_BleSetDefaultPhy
 *
 * Description      To set preferred PHY for ensuing LE connections
 *
 *
 * Returns          BTM_SUCCESS if command successfully sent to controller,
 *                  BTM_MODE_UNSUPPORTED if local controller doesn't support LE
 *                  2M or LE Coded PHY
 *
 ******************************************************************************/
tBTM_STATUS BTM_BleSetDefaultPhy(uint8_t all_phys, uint8_t tx_phys,
                                 uint8_t rx_phys) {
  BTM_TRACE_DEBUG("%s: all_phys = 0x%02x, tx_phys = 0x%02x, rx_phys = 0x%02x",
                  __func__, all_phys, tx_phys, rx_phys);

  // checking if local controller supports it!
  if (!controller_get_interface()->supports_ble_2m_phy() &&
      !controller_get_interface()->supports_ble_coded_phy()) {
    BTM_TRACE_ERROR("%s failed, request not supported in local controller!",
                    __func__);
    return BTM_MODE_UNSUPPORTED;
  }

  const uint8_t len = HCIC_PARAM_SIZE_BLE_SET_DEFAULT_PHY;
  uint8_t data[len];
  uint8_t* pp = data;
  UINT8_TO_STREAM(pp, all_phys);
  UINT8_TO_STREAM(pp, tx_phys);
  UINT8_TO_STREAM(pp, rx_phys);
  btu_hcif_send_cmd_with_cb(FROM_HERE, HCI_BLE_SET_DEFAULT_PHY, data, len,
                            base::Bind(doNothing));
  return BTM_SUCCESS;
}

/*******************************************************************************
 *
 * Function         BTM_BleSetPhy
 *
 * Description      To set PHY preferences for specified LE connection
 *
 *
 * Returns          BTM_SUCCESS if command successfully sent to controller,
 *                  BTM_MODE_UNSUPPORTED if local controller doesn't support LE
 *                  2M or LE Coded PHY,
 *                  BTM_ILLEGAL_VALUE if specified remote doesn't support LE 2M
 *                  or LE Coded PHY,
 *                  BTM_WRONG_MODE if Device in wrong mode for request.
 *
 ******************************************************************************/
void BTM_BleSetPhy(const RawAddress& bd_addr, uint8_t tx_phys, uint8_t rx_phys,
                   uint16_t phy_options) {
  tACL_CONN* p_acl = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);

  if (p_acl == NULL) {
    BTM_TRACE_ERROR("%s: Wrong mode: no LE link exist or LE not supported",
                    __func__);
    return;
  }

  uint8_t all_phys = 0;
  if (tx_phys == 0) all_phys &= 0x01;
  if (rx_phys == 0) all_phys &= 0x02;

  BTM_TRACE_DEBUG(
      "%s: all_phys = 0x%02x, tx_phys = 0x%02x, rx_phys = 0x%02x, phy_options "
      "= 0x%04x",
      __func__, all_phys, tx_phys, rx_phys, phy_options);

  uint16_t handle = p_acl->hci_handle;

  // checking if local controller supports it!
  if (!controller_get_interface()->supports_ble_2m_phy() &&
      !controller_get_interface()->supports_ble_coded_phy()) {
    BTM_TRACE_ERROR("%s failed, request not supported in local controller!",
                    __func__);
    gatt_notify_phy_updated(GATT_REQ_NOT_SUPPORTED, handle, tx_phys, rx_phys);
    return;
  }

  if (!HCI_LE_2M_PHY_SUPPORTED(p_acl->peer_le_features) &&
      !HCI_LE_CODED_PHY_SUPPORTED(p_acl->peer_le_features)) {
    BTM_TRACE_ERROR("%s failed, peer does not support request", __func__);
    gatt_notify_phy_updated(GATT_REQ_NOT_SUPPORTED, handle, tx_phys, rx_phys);
    return;
  }

  const uint8_t len = HCIC_PARAM_SIZE_BLE_SET_PHY;
  uint8_t data[len];
  uint8_t* pp = data;
  UINT16_TO_STREAM(pp, handle);
  UINT8_TO_STREAM(pp, all_phys);
  UINT8_TO_STREAM(pp, tx_phys);
  UINT8_TO_STREAM(pp, rx_phys);
  UINT16_TO_STREAM(pp, phy_options);
  btu_hcif_send_cmd_with_cb(FROM_HERE, HCI_BLE_SET_PHY, data, len,
                            base::Bind(doNothing));
}

/*******************************************************************************
 *
 * Function         BTM_BleGetLTK
 *
 * Description      This function returns peer LTK of the LE connection.
 *
 * Parameter        bdaddr: remote device address
 *
 * Returns          Octet16 (LTK)
 *
 ******************************************************************************/
Octet16 BTM_BleGetLTK(const RawAddress& bd_addr) {
  Octet16 ltk = {};
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bd_addr);

  if (p_rec == NULL) {
    BTM_TRACE_ERROR("BTM_BleGetLTK: request received for unknown device");
    return ltk;
  }

  tACL_CONN* p_acl = btm_bda_to_acl(bd_addr, BT_TRANSPORT_LE);

  if (p_acl == NULL || p_acl->transport != BT_TRANSPORT_LE) {
    BTM_TRACE_ERROR("%s: no LE link exist or not a LE link", __func__);
    return ltk;
  }

  if (p_rec->ble.key_type && (p_rec->sec_flags & BTM_SEC_LE_LINK_KEY_KNOWN)) {
    ltk = p_rec->ble.keys.pltk;
  }
  return ltk;
}

/*******************************************************************************
 *
 * Function         btm_ble_determine_security_act
 *
 * Description      This function checks the security of current LE link
 *                  and returns the appropriate action that needs to be
 *                  taken to achieve the required security.
 *
 * Parameter        is_originator - True if outgoing connection
 *                  bdaddr: remote device address
 *                  security_required: Security required for the service.
 *
 * Returns          The appropriate security action required.
 *
 ******************************************************************************/
tBTM_SEC_ACTION btm_ble_determine_security_act(bool is_originator,
                                               const RawAddress& bdaddr,
                                               uint16_t security_required) {
  tBTM_LE_AUTH_REQ auth_req = 0x00;

  if (is_originator) {
    if ((security_required & BTM_SEC_OUT_FLAGS) == 0 &&
        (security_required & BTM_SEC_OUT_MITM) == 0) {
      BTM_TRACE_DEBUG("%s No security required for outgoing connection",
                      __func__);
      return BTM_SEC_OK;
    }

    if (security_required & BTM_SEC_OUT_MITM) auth_req |= BTM_LE_AUTH_REQ_MITM;
  } else {
    if ((security_required & BTM_SEC_IN_FLAGS) == 0 &&
        (security_required & BTM_SEC_IN_MITM) == 0) {
      BTM_TRACE_DEBUG("%s No security required for incoming connection",
                      __func__);
      return BTM_SEC_OK;
    }

    if (security_required & BTM_SEC_IN_MITM) auth_req |= BTM_LE_AUTH_REQ_MITM;
  }

  tBTM_BLE_SEC_REQ_ACT ble_sec_act;
  btm_ble_link_sec_check(bdaddr, auth_req, &ble_sec_act);

  BTM_TRACE_DEBUG("%s ble_sec_act %d", __func__, ble_sec_act);

  if (ble_sec_act == BTM_BLE_SEC_REQ_ACT_DISCARD) return BTM_SEC_ENC_PENDING;

  if (ble_sec_act == BTM_BLE_SEC_REQ_ACT_NONE) return BTM_SEC_OK;

  uint8_t sec_flag = 0;
  BTM_GetSecurityFlagsByTransport(bdaddr, &sec_flag, BT_TRANSPORT_LE);

  bool is_link_encrypted = false;
  bool is_key_mitm = false;
  if (sec_flag & (BTM_SEC_FLAG_ENCRYPTED | BTM_SEC_FLAG_LKEY_KNOWN)) {
    if (sec_flag & BTM_SEC_FLAG_ENCRYPTED) is_link_encrypted = true;

    if (sec_flag & BTM_SEC_FLAG_LKEY_AUTHED) is_key_mitm = true;
  }

  if (auth_req & BTM_LE_AUTH_REQ_MITM) {
    if (!is_key_mitm) {
      return BTM_SEC_ENCRYPT_MITM;
    } else {
      if (is_link_encrypted)
        return BTM_SEC_OK;
      else
        return BTM_SEC_ENCRYPT;
    }
  } else {
    if (is_link_encrypted)
      return BTM_SEC_OK;
    else
      return BTM_SEC_ENCRYPT_NO_MITM;
  }

  return BTM_SEC_OK;
}

/*******************************************************************************
 *
 * Function         btm_ble_start_sec_check
 *
 * Description      This function is to check and set the security required for
 *                  LE link for LE COC.
 *
 * Parameter        bdaddr: remote device address.
 *                  psm : PSM of the LE COC sevice.
 *                  is_originator: true if outgoing connection.
 *                  p_callback : Pointer to the callback function.
 *                  p_ref_data : Pointer to be returned along with the callback.
 *
 * Returns          Returns  - L2CAP LE Connection Response Result Code.
 *
 ******************************************************************************/
tL2CAP_LE_RESULT_CODE btm_ble_start_sec_check(const RawAddress& bd_addr,
                                              uint16_t psm, bool is_originator,
                                              tBTM_SEC_CALLBACK* p_callback,
                                              void* p_ref_data) {
  /* Find the service record for the PSM */
  tBTM_SEC_SERV_REC* p_serv_rec = btm_sec_find_first_serv(is_originator, psm);

  /* If there is no application registered with this PSM do not allow connection
   */
  if (!p_serv_rec) {
    BTM_TRACE_WARNING("%s PSM: %d no application registerd", __func__, psm);
    (*p_callback)(&bd_addr, BT_TRANSPORT_LE, p_ref_data, BTM_MODE_UNSUPPORTED);
    return L2CAP_LE_RESULT_NO_PSM;
  }
  uint8_t sec_flag = 0;
  BTM_GetSecurityFlagsByTransport(bd_addr, &sec_flag, BT_TRANSPORT_LE);

  if (!is_originator) {
    if ((p_serv_rec->security_flags & BTM_SEC_IN_ENCRYPT) &&
        !(sec_flag & BTM_SEC_ENCRYPTED)) {
      BTM_TRACE_ERROR(
          "%s: L2CAP_LE_RESULT_INSUFFICIENT_ENCRYP. service "
          "security_flags=0x%x, "
          "sec_flag=0x%x",
          __func__, p_serv_rec->security_flags, sec_flag);
      return L2CAP_LE_RESULT_INSUFFICIENT_ENCRYP;
    } else if ((p_serv_rec->security_flags & BTM_SEC_IN_AUTHENTICATE) &&
               !(sec_flag &
                 (BTM_SEC_LINK_KEY_AUTHED | BTM_SEC_AUTHENTICATED))) {
      BTM_TRACE_ERROR(
          "%s: L2CAP_LE_RESULT_INSUFFICIENT_AUTHENTICATION. service "
          "security_flags=0x%x, "
          "sec_flag=0x%x",
          __func__, p_serv_rec->security_flags, sec_flag);
      return L2CAP_LE_RESULT_INSUFFICIENT_AUTHENTICATION;
    }
    /* TODO: When security is required, then must check that the key size of our
       service is equal or smaller than the incoming connection key size. */
  }

  tBTM_SEC_ACTION sec_act = btm_ble_determine_security_act(
      is_originator, bd_addr, p_serv_rec->security_flags);

  tBTM_BLE_SEC_ACT ble_sec_act = BTM_BLE_SEC_NONE;
  tL2CAP_LE_RESULT_CODE result = L2CAP_LE_RESULT_CONN_OK;

  switch (sec_act) {
    case BTM_SEC_OK:
      BTM_TRACE_DEBUG("%s Security met", __func__);
      p_callback(&bd_addr, BT_TRANSPORT_LE, p_ref_data, BTM_SUCCESS);
      result = L2CAP_LE_RESULT_CONN_OK;
      break;

    case BTM_SEC_ENCRYPT:
      BTM_TRACE_DEBUG("%s Encryption needs to be done", __func__);
      ble_sec_act = BTM_BLE_SEC_ENCRYPT;
      break;

    case BTM_SEC_ENCRYPT_MITM:
      BTM_TRACE_DEBUG("%s Pairing with MITM needs to be done", __func__);
      ble_sec_act = BTM_BLE_SEC_ENCRYPT_MITM;
      break;

    case BTM_SEC_ENCRYPT_NO_MITM:
      BTM_TRACE_DEBUG("%s Pairing with No MITM needs to be done", __func__);
      ble_sec_act = BTM_BLE_SEC_ENCRYPT_NO_MITM;
      break;

    case BTM_SEC_ENC_PENDING:
      BTM_TRACE_DEBUG("%s Ecryption pending", __func__);
      ble_sec_act = BTM_BLE_SEC_ENCRYPT;
      break;
  }

  if (ble_sec_act == BTM_BLE_SEC_NONE) return result;

  tL2C_LCB* p_lcb = l2cu_find_lcb_by_bd_addr(bd_addr, BT_TRANSPORT_LE);
  p_lcb->sec_act = sec_act;
  BTM_SetEncryption(bd_addr, BT_TRANSPORT_LE, p_callback, p_ref_data,
                    ble_sec_act);

  return L2CAP_LE_RESULT_CONN_OK;
}

/*******************************************************************************
 *
 * Function         btm_ble_rand_enc_complete
 *
 * Description      This function is the callback functions for HCI_Rand command
 *                  and HCI_Encrypt command is completed.
 *                  This message is received from the HCI.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_rand_enc_complete(uint8_t* p, uint16_t evt_len, uint16_t op_code,
                               tBTM_RAND_ENC_CB* p_enc_cplt_cback) {
  tBTM_RAND_ENC params;
  uint8_t* p_dest = params.param_buf;

  BTM_TRACE_DEBUG("btm_ble_rand_enc_complete");

  memset(&params, 0, sizeof(tBTM_RAND_ENC));

  /* If there was a callback address for vcs complete, call it */
  if (p_enc_cplt_cback && p) {

    if (evt_len < 1) {
      goto err_out;
    }

    /* Pass paramters to the callback function */
    STREAM_TO_UINT8(params.status, p); /* command status */

    if (params.status == HCI_SUCCESS) {
      params.opcode = op_code;

      if (op_code == HCI_BLE_RAND)
        params.param_len = BT_OCTET8_LEN;
      else
        params.param_len = OCTET16_LEN;
      if (evt_len < 1 + params.param_len) {
        goto err_out;
      }

      /* Fetch return info from HCI event message */
      memcpy(p_dest, p, params.param_len);
    }
    if (p_enc_cplt_cback) /* Call the Encryption complete callback function */
      (*p_enc_cplt_cback)(&params);
  }

  return;

err_out:
  BTM_TRACE_ERROR("%s malformatted event packet, too short", __func__);
}

/*******************************************************************************
 *
 * Function         btm_ble_get_enc_key_type
 *
 * Description      This function is to increment local sign counter
 * Returns         None
 *
 ******************************************************************************/
void btm_ble_increment_sign_ctr(const RawAddress& bd_addr, bool is_local) {
  tBTM_SEC_DEV_REC* p_dev_rec;

  BTM_TRACE_DEBUG("btm_ble_increment_sign_ctr is_local=%d", is_local);

  p_dev_rec = btm_find_dev(bd_addr);
  if (p_dev_rec != NULL) {
    if (is_local)
      p_dev_rec->ble.keys.local_counter++;
    else
      p_dev_rec->ble.keys.counter++;
    BTM_TRACE_DEBUG("is_local=%d local sign counter=%d peer sign counter=%d",
                    is_local, p_dev_rec->ble.keys.local_counter,
                    p_dev_rec->ble.keys.counter);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_get_enc_key_type
 *
 * Description      This function is to get the BLE key type that has been
 *                  exchanged betweem the local device and the peer device.
 *
 * Returns          p_key_type: output parameter to carry the key type value.
 *
 ******************************************************************************/
bool btm_ble_get_enc_key_type(const RawAddress& bd_addr, uint8_t* p_key_types) {
  tBTM_SEC_DEV_REC* p_dev_rec;

  BTM_TRACE_DEBUG("btm_ble_get_enc_key_type");

  p_dev_rec = btm_find_dev(bd_addr);
  if (p_dev_rec != NULL) {
    *p_key_types = p_dev_rec->ble.key_type;
    return true;
  }
  return false;
}

/*******************************************************************************
 *
 * Function         btm_get_local_div
 *
 * Description      This function is called to read the local DIV
 *
 * Returns          TURE - if a valid DIV is availavle
 ******************************************************************************/
bool btm_get_local_div(const RawAddress& bd_addr, uint16_t* p_div) {
  tBTM_SEC_DEV_REC* p_dev_rec;
  bool status = false;
  VLOG(1) << __func__ << " bd_addr: " << bd_addr;

  *p_div = 0;
  p_dev_rec = btm_find_dev(bd_addr);

  if (p_dev_rec && p_dev_rec->ble.keys.div) {
    status = true;
    *p_div = p_dev_rec->ble.keys.div;
  }
  BTM_TRACE_DEBUG("btm_get_local_div status=%d (1-OK) DIV=0x%x", status,
                  *p_div);
  return status;
}

/*******************************************************************************
 *
 * Function         btm_sec_save_le_key
 *
 * Description      This function is called by the SMP to update
 *                  an  BLE key.  SMP is internal, whereas all the keys shall
 *                  be sent to the application.  The function is also called
 *                  when application passes ble key stored in NVRAM to the
 *                  btm_sec.
 *                  pass_to_application parameter is false in this case.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_sec_save_le_key(const RawAddress& bd_addr, tBTM_LE_KEY_TYPE key_type,
                         tBTM_LE_KEY_VALUE* p_keys, bool pass_to_application) {
  tBTM_SEC_DEV_REC* p_rec;
  tBTM_LE_EVT_DATA cb_data;

  BTM_TRACE_DEBUG("btm_sec_save_le_key key_type=0x%x pass_to_application=%d",
                  key_type, pass_to_application);
  /* Store the updated key in the device database */

  VLOG(1) << "bd_addr:" << bd_addr;

  if ((p_rec = btm_find_dev(bd_addr)) != NULL &&
      (p_keys || key_type == BTM_LE_KEY_LID)) {
    btm_ble_init_pseudo_addr(p_rec, bd_addr);

    switch (key_type) {
      case BTM_LE_KEY_PENC:
        p_rec->ble.keys.pltk = p_keys->penc_key.ltk;
        memcpy(p_rec->ble.keys.rand, p_keys->penc_key.rand, BT_OCTET8_LEN);
        p_rec->ble.keys.sec_level = p_keys->penc_key.sec_level;
        p_rec->ble.keys.ediv = p_keys->penc_key.ediv;
        p_rec->ble.keys.key_size = p_keys->penc_key.key_size;
        p_rec->ble.key_type |= BTM_LE_KEY_PENC;
        p_rec->sec_flags |= BTM_SEC_LE_LINK_KEY_KNOWN;
        if (p_keys->penc_key.sec_level == SMP_SEC_AUTHENTICATED)
          p_rec->sec_flags |= BTM_SEC_LE_LINK_KEY_AUTHED;
        else
          p_rec->sec_flags &= ~BTM_SEC_LE_LINK_KEY_AUTHED;
        BTM_TRACE_DEBUG(
            "BTM_LE_KEY_PENC key_type=0x%x sec_flags=0x%x sec_leve=0x%x",
            p_rec->ble.key_type, p_rec->sec_flags, p_rec->ble.keys.sec_level);
        break;

      case BTM_LE_KEY_PID:
        p_rec->ble.keys.irk = p_keys->pid_key.irk;
        p_rec->ble.identity_addr = p_keys->pid_key.identity_addr;
        p_rec->ble.identity_addr_type = p_keys->pid_key.identity_addr_type;
        p_rec->ble.key_type |= BTM_LE_KEY_PID;
        BTM_TRACE_DEBUG(
            "%s: BTM_LE_KEY_PID key_type=0x%x save peer IRK, change bd_addr=%s "
            "to id_addr=%s id_addr_type=0x%x",
            __func__, p_rec->ble.key_type, p_rec->bd_addr.ToString().c_str(),
            p_keys->pid_key.identity_addr.ToString().c_str(),
            p_keys->pid_key.identity_addr_type);
#ifdef ADV_AUDIO_FEATURE
        if (btm_cb.api.p_le_id_addr_callback) {
          (*btm_cb.api.p_le_id_addr_callback)
            (p_rec->bd_addr, p_keys->pid_key.identity_addr);
        }
#endif
        /* update device record address as identity address */
        p_rec->bd_addr = p_keys->pid_key.identity_addr;
        /* combine DUMO device security record if needed */
        btm_consolidate_dev(p_rec);
        break;

      case BTM_LE_KEY_PCSRK:
        p_rec->ble.keys.pcsrk = p_keys->pcsrk_key.csrk;
        p_rec->ble.keys.srk_sec_level = p_keys->pcsrk_key.sec_level;
        p_rec->ble.keys.counter = p_keys->pcsrk_key.counter;
        p_rec->ble.key_type |= BTM_LE_KEY_PCSRK;
        p_rec->sec_flags |= BTM_SEC_LE_LINK_KEY_KNOWN;
        if (p_keys->pcsrk_key.sec_level == SMP_SEC_AUTHENTICATED)
          p_rec->sec_flags |= BTM_SEC_LE_LINK_KEY_AUTHED;
        else
          p_rec->sec_flags &= ~BTM_SEC_LE_LINK_KEY_AUTHED;

        BTM_TRACE_DEBUG(
            "BTM_LE_KEY_PCSRK key_type=0x%x sec_flags=0x%x sec_level=0x%x "
            "peer_counter=%d",
            p_rec->ble.key_type, p_rec->sec_flags,
            p_rec->ble.keys.srk_sec_level, p_rec->ble.keys.counter);
        break;

      case BTM_LE_KEY_LENC:
        p_rec->ble.keys.lltk = p_keys->lenc_key.ltk;
        p_rec->ble.keys.div = p_keys->lenc_key.div; /* update DIV */
        p_rec->ble.keys.sec_level = p_keys->lenc_key.sec_level;
        p_rec->ble.keys.key_size = p_keys->lenc_key.key_size;
        p_rec->ble.key_type |= BTM_LE_KEY_LENC;

        BTM_TRACE_DEBUG(
            "BTM_LE_KEY_LENC key_type=0x%x DIV=0x%x key_size=0x%x "
            "sec_level=0x%x",
            p_rec->ble.key_type, p_rec->ble.keys.div, p_rec->ble.keys.key_size,
            p_rec->ble.keys.sec_level);
        break;

      case BTM_LE_KEY_LCSRK: /* local CSRK has been delivered */
        p_rec->ble.keys.lcsrk = p_keys->lcsrk_key.csrk;
        p_rec->ble.keys.div = p_keys->lcsrk_key.div; /* update DIV */
        p_rec->ble.keys.local_csrk_sec_level = p_keys->lcsrk_key.sec_level;
        p_rec->ble.keys.local_counter = p_keys->lcsrk_key.counter;
        p_rec->ble.key_type |= BTM_LE_KEY_LCSRK;
        BTM_TRACE_DEBUG(
            "BTM_LE_KEY_LCSRK key_type=0x%x DIV=0x%x scrk_sec_level=0x%x "
            "local_counter=%d",
            p_rec->ble.key_type, p_rec->ble.keys.div,
            p_rec->ble.keys.local_csrk_sec_level,
            p_rec->ble.keys.local_counter);
        break;

      case BTM_LE_KEY_LID:
        p_rec->ble.key_type |= BTM_LE_KEY_LID;
        break;
      default:
        BTM_TRACE_WARNING("btm_sec_save_le_key (Bad key_type 0x%02x)",
                          key_type);
        return;
    }

    VLOG(1) << "BLE key type 0x" << loghex(key_type)
            << " updated for BDA: " << bd_addr << " (btm_sec_save_le_key)";

    /* Notify the application that one of the BLE keys has been updated
       If link key is in progress, it will get sent later.*/
    if (pass_to_application && btm_cb.api.p_le_callback) {
      cb_data.key.p_key_value = p_keys;
      cb_data.key.key_type = key_type;

      (*btm_cb.api.p_le_callback)(BTM_LE_KEY_EVT, bd_addr, &cb_data);
    }
    return;
  }

  LOG(WARNING) << "BLE key type 0x" << loghex(key_type)
               << " called for Unknown BDA or type: " << bd_addr
               << "(btm_sec_save_le_key)";

  if (p_rec) {
    BTM_TRACE_DEBUG("sec_flags=0x%x", p_rec->sec_flags);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_update_sec_key_size
 *
 * Description      update the current lin kencryption key size
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_update_sec_key_size(const RawAddress& bd_addr,
                                 uint8_t enc_key_size) {
  tBTM_SEC_DEV_REC* p_rec;

  BTM_TRACE_DEBUG("btm_ble_update_sec_key_size enc_key_size = %d",
                  enc_key_size);

  p_rec = btm_find_dev(bd_addr);
  if (p_rec != NULL) {
    p_rec->enc_key_size = enc_key_size;
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_read_sec_key_size
 *
 * Description      update the current lin kencryption key size
 *
 * Returns          void
 *
 ******************************************************************************/
uint8_t btm_ble_read_sec_key_size(const RawAddress& bd_addr) {
  tBTM_SEC_DEV_REC* p_rec;

  p_rec = btm_find_dev(bd_addr);
  if (p_rec != NULL) {
    return p_rec->enc_key_size;
  } else
    return 0;
}

/*******************************************************************************
 *
 * Function         btm_ble_link_sec_check
 *
 * Description      Check BLE link security level match.
 *
 * Returns          true: check is OK and the *p_sec_req_act contain the action
 *
 ******************************************************************************/
void btm_ble_link_sec_check(const RawAddress& bd_addr,
                            tBTM_LE_AUTH_REQ auth_req,
                            tBTM_BLE_SEC_REQ_ACT* p_sec_req_act) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  uint8_t req_sec_level = BTM_LE_SEC_NONE, cur_sec_level = BTM_LE_SEC_NONE;
  bool le_fresh_pairing_enabled = false;

  BTM_TRACE_DEBUG("btm_ble_link_sec_check auth_req =0x%x", auth_req);

  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("btm_ble_link_sec_check received for unknown device");
    return;
  }

  if (p_dev_rec->sec_state == BTM_SEC_STATE_ENCRYPTING ||
      p_dev_rec->sec_state == BTM_SEC_STATE_AUTHENTICATING) {
    /* race condition: discard the security request while master is encrypting
     * the link */
    *p_sec_req_act = BTM_BLE_SEC_REQ_ACT_DISCARD;
  } else {
    req_sec_level = BTM_LE_SEC_UNAUTHENTICATE;
    if (auth_req & BTM_LE_AUTH_REQ_MITM) {
      req_sec_level = BTM_LE_SEC_AUTHENTICATED;
    }

    BTM_TRACE_DEBUG("dev_rec sec_flags=0x%x", p_dev_rec->sec_flags);

    /* currently encrpted  */
    if (p_dev_rec->sec_flags & BTM_SEC_LE_ENCRYPTED) {
      if (p_dev_rec->sec_flags & BTM_SEC_LE_AUTHENTICATED)
        cur_sec_level = BTM_LE_SEC_AUTHENTICATED;
      else
        cur_sec_level = BTM_LE_SEC_UNAUTHENTICATE;
    } else /* unencrypted link */
    {
      /* if bonded, get the key security level */
      if (p_dev_rec->ble.key_type & BTM_LE_KEY_PENC)
        cur_sec_level = p_dev_rec->ble.keys.sec_level;
      else
        cur_sec_level = BTM_LE_SEC_NONE;
    }

    if(stack_config_get_interface()->get_pts_le_fresh_pairing_enabled()) {
      if (cur_sec_level == req_sec_level) {
        BTM_TRACE_DEBUG("LE fresh pairing enabled");
        le_fresh_pairing_enabled = true;
      }
    }

    if (!le_fresh_pairing_enabled && (cur_sec_level >= req_sec_level)) {
      /* To avoid re-encryption on an encrypted link for an equal condition
       * encryption */
      *p_sec_req_act = BTM_BLE_SEC_REQ_ACT_ENCRYPT;
    } else {
      /* start the pariring process to upgrade the keys*/
      *p_sec_req_act = BTM_BLE_SEC_REQ_ACT_PAIR;
    }
  }

  BTM_TRACE_DEBUG("cur_sec_level=%d req_sec_level=%d sec_req_act=%d",
                  cur_sec_level, req_sec_level, *p_sec_req_act);
}

/*******************************************************************************
 *
 * Function         btm_ble_set_encryption
 *
 * Description      This function is called to ensure that LE connection is
 *                  encrypted.  Should be called only on an open connection.
 *                  Typically only needed for connections that first want to
 *                  bring up unencrypted links, then later encrypt them.
 *
 * Returns          void
 *                  the local device ER is copied into er
 *
 ******************************************************************************/
tBTM_STATUS btm_ble_set_encryption(const RawAddress& bd_addr,
                                   tBTM_BLE_SEC_ACT sec_act,
                                   uint8_t link_role) {
  tBTM_STATUS cmd = BTM_NO_RESOURCES;
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bd_addr);
  tBTM_BLE_SEC_REQ_ACT sec_req_act;
  tBTM_LE_AUTH_REQ auth_req;

  if (p_rec == NULL) {
    BTM_TRACE_WARNING(
        "btm_ble_set_encryption (NULL device record!! sec_act=0x%x", sec_act);
    return (BTM_WRONG_MODE);
  }

  BTM_TRACE_DEBUG("btm_ble_set_encryption sec_act=0x%x role_master=%d", sec_act,
                  p_rec->role_master);

  if (sec_act == BTM_BLE_SEC_ENCRYPT_MITM) {
    p_rec->security_required |= BTM_SEC_IN_MITM;
  }

  switch (sec_act) {
    case BTM_BLE_SEC_ENCRYPT:
      if (link_role == BTM_ROLE_MASTER) {
        /* start link layer encryption using the security info stored */
        cmd = btm_ble_start_encrypt(bd_addr, false, NULL);
        break;
      }
    /* if salve role then fall through to call SMP_Pair below which will send a
       sec_request to request the master to encrypt the link */
      FALLTHROUGH_INTENDED; /* FALLTHROUGH */
    case BTM_BLE_SEC_ENCRYPT_NO_MITM:
    case BTM_BLE_SEC_ENCRYPT_MITM:
      auth_req = (sec_act == BTM_BLE_SEC_ENCRYPT_NO_MITM)
                     ? SMP_AUTH_BOND
                     : (SMP_AUTH_BOND | SMP_AUTH_YN_BIT);
      btm_ble_link_sec_check(bd_addr, auth_req, &sec_req_act);
      if (sec_req_act == BTM_BLE_SEC_REQ_ACT_NONE ||
          sec_req_act == BTM_BLE_SEC_REQ_ACT_DISCARD) {
        BTM_TRACE_DEBUG("%s, no action needed. Ignore", __func__);
        cmd = BTM_SUCCESS;
        break;
      }
      if (link_role == BTM_ROLE_MASTER) {
        if (sec_req_act == BTM_BLE_SEC_REQ_ACT_ENCRYPT) {
          cmd = btm_ble_start_encrypt(bd_addr, false, NULL);
          break;
        }
      }
      if(!stack_config_get_interface()->get_pts_le_sec_request_disabled()) {
        if (SMP_Pair(bd_addr) == SMP_STARTED) {
          cmd = BTM_CMD_STARTED;
          p_rec->sec_state = BTM_SEC_STATE_AUTHENTICATING;
        }
      }
      break;

    default:
      cmd = BTM_WRONG_MODE;
      break;
  }
  return cmd;
}

/*******************************************************************************
 *
 * Function         btm_ble_ltk_request
 *
 * Description      This function is called when encryption request is received
 *                  on a slave device.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_ltk_request(uint16_t handle, uint8_t rand[8], uint16_t ediv) {
  tBTM_CB* p_cb = &btm_cb;
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev_by_handle(handle);

  BTM_TRACE_DEBUG("btm_ble_ltk_request");

  p_cb->ediv = ediv;

  memcpy(p_cb->enc_rand, rand, BT_OCTET8_LEN);

  if (p_dev_rec != NULL) {
    if (!smp_proc_ltk_request(p_dev_rec->bd_addr)) {
      btm_ble_ltk_request_reply(p_dev_rec->bd_addr, false, Octet16{0});
    }
  }
}

/** This function is called to start LE encryption.
 * Returns BTM_SUCCESS if encryption was started successfully
 */
tBTM_STATUS btm_ble_start_encrypt(const RawAddress& bda, bool use_stk,
                                  Octet16* p_stk) {
  tBTM_CB* p_cb = &btm_cb;
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bda);
  BT_OCTET8 dummy_rand = {0};

  BTM_TRACE_DEBUG("btm_ble_start_encrypt");

  if (!p_rec) {
    BTM_TRACE_ERROR("Link is not active, can not encrypt!");
    return BTM_WRONG_MODE;
  }

  if (p_rec->sec_state == BTM_SEC_STATE_ENCRYPTING) {
    BTM_TRACE_WARNING("Link Encryption is active, Busy!");
    return BTM_BUSY;
  }

  p_cb->enc_handle = p_rec->ble_hci_handle;

  if (use_stk) {
    btsnd_hcic_ble_start_enc(p_rec->ble_hci_handle, dummy_rand, 0, *p_stk);
  } else if (p_rec->ble.key_type & BTM_LE_KEY_PENC) {
    btsnd_hcic_ble_start_enc(p_rec->ble_hci_handle, p_rec->ble.keys.rand,
                             p_rec->ble.keys.ediv, p_rec->ble.keys.pltk);
  } else {
    BTM_TRACE_ERROR("No key available to encrypt the link");
    return BTM_NO_RESOURCES;
  }

  if (p_rec->sec_state == BTM_SEC_STATE_IDLE) {
    p_rec->sec_state = BTM_SEC_STATE_ENCRYPTING;
    p_rec->is_le_enc_in_progress = true;
  }

  return BTM_CMD_STARTED;
}

/*******************************************************************************
 *
 * Function         btm_ble_link_encrypted
 *
 * Description      This function is called when LE link encrption status is
 *                  changed.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_link_encrypted(const RawAddress& bd_addr, uint8_t encr_enable) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  bool enc_cback;

  if (!p_dev_rec) {
    BTM_TRACE_WARNING(
        "btm_ble_link_encrypted (No Device Found!) encr_enable=%d",
        encr_enable);
    return;
  }

  BTM_TRACE_DEBUG("btm_ble_link_encrypted encr_enable=%d", encr_enable);

  enc_cback = (p_dev_rec->sec_state == BTM_SEC_STATE_ENCRYPTING);

  smp_link_encrypted(bd_addr, encr_enable);

  BTM_TRACE_DEBUG(" p_dev_rec->sec_flags=0x%x", p_dev_rec->sec_flags);

  if (encr_enable && p_dev_rec->enc_key_size == 0)
    p_dev_rec->enc_key_size = p_dev_rec->ble.keys.key_size;

  p_dev_rec->sec_state = BTM_SEC_STATE_IDLE;
  p_dev_rec->is_le_enc_in_progress = false;
  if (p_dev_rec->p_callback && enc_cback) {
    if (encr_enable)
      btm_sec_dev_rec_cback_event(p_dev_rec, BTM_SUCCESS, true);
    else if (p_dev_rec->sec_flags & ~BTM_SEC_LE_LINK_KEY_KNOWN) {
      btm_sec_dev_rec_cback_event(p_dev_rec, BTM_FAILED_ON_SECURITY, true);
    } else if (p_dev_rec->role_master)
      btm_sec_dev_rec_cback_event(p_dev_rec, BTM_ERR_PROCESSING, true);
  }
  /* to notify GATT to send data if any request is pending */
  gatt_notify_enc_cmpl(p_dev_rec->ble.pseudo_addr);

  /* Update EATT support */
  if (encr_enable)
    gatt_update_eatt_support(p_dev_rec->ble.pseudo_addr);
}

/*******************************************************************************
 *
 * Function         btm_ble_ltk_request_reply
 *
 * Description      This function is called to send a LTK request reply on a
 *                  slave
 *                  device.
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_ltk_request_reply(const RawAddress& bda, bool use_stk,
                               const Octet16& stk) {
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bda);
  tBTM_CB* p_cb = &btm_cb;

  if (p_rec == NULL) {
    BTM_TRACE_ERROR("btm_ble_ltk_request_reply received for unknown device");
    return;
  }

  BTM_TRACE_DEBUG("btm_ble_ltk_request_reply");
  p_cb->enc_handle = p_rec->ble_hci_handle;
  p_cb->key_size = p_rec->ble.keys.key_size;

  BTM_TRACE_ERROR("key size = %d", p_rec->ble.keys.key_size);
  if (use_stk) {
    btsnd_hcic_ble_ltk_req_reply(btm_cb.enc_handle, stk);
  } else /* calculate LTK using peer device  */
  {
    if (p_rec->ble.key_type & BTM_LE_KEY_LENC)
      btsnd_hcic_ble_ltk_req_reply(btm_cb.enc_handle, p_rec->ble.keys.lltk);
    else
      btsnd_hcic_ble_ltk_req_neg_reply(btm_cb.enc_handle);
  }
}

/*******************************************************************************
 *
 * Function         btm_ble_io_capabilities_req
 *
 * Description      This function is called to handle SMP get IO capability
 *                  request.
 *
 * Returns          void
 *
 ******************************************************************************/
uint8_t btm_ble_io_capabilities_req(tBTM_SEC_DEV_REC* p_dev_rec,
                                    tBTM_LE_IO_REQ* p_data) {
  uint8_t callback_rc = BTM_SUCCESS;
  BTM_TRACE_DEBUG("btm_ble_io_capabilities_req");
  if (btm_cb.api.p_le_callback) {
    /* the callback function implementation may change the IO capability... */
    callback_rc = (*btm_cb.api.p_le_callback)(
        BTM_LE_IO_REQ_EVT, p_dev_rec->bd_addr, (tBTM_LE_EVT_DATA*)p_data);
  }
  if ((callback_rc == BTM_SUCCESS) || (BTM_OOB_UNKNOWN != p_data->oob_data)) {
#if (BTM_BLE_CONFORMANCE_TESTING == TRUE)
    if (btm_cb.devcb.keep_rfu_in_auth_req) {
      BTM_TRACE_DEBUG("btm_ble_io_capabilities_req keep_rfu_in_auth_req = %u",
                      btm_cb.devcb.keep_rfu_in_auth_req);
      p_data->auth_req &= BTM_LE_AUTH_REQ_MASK_KEEP_RFU;
      btm_cb.devcb.keep_rfu_in_auth_req = false;
    } else { /* default */
      p_data->auth_req &= BTM_LE_AUTH_REQ_MASK;
    }
#else
    p_data->auth_req &= BTM_LE_AUTH_REQ_MASK;
#endif

    BTM_TRACE_DEBUG(
        "btm_ble_io_capabilities_req 1: p_dev_rec->security_required = %d "
        "auth_req:%d",
        p_dev_rec->security_required, p_data->auth_req);
    BTM_TRACE_DEBUG(
        "btm_ble_io_capabilities_req 2: i_keys=0x%x r_keys=0x%x (bit 0-LTK "
        "1-IRK 2-CSRK)",
        p_data->init_keys, p_data->resp_keys);

    /* if authentication requires MITM protection, put on the mask */
    if (p_dev_rec->security_required & BTM_SEC_IN_MITM)
      p_data->auth_req |= BTM_LE_AUTH_REQ_MITM;

    if (!(p_data->auth_req & SMP_AUTH_BOND)) {
      BTM_TRACE_DEBUG("Non bonding: No keys should be exchanged");
      p_data->init_keys = 0;
      p_data->resp_keys = 0;
    }

    BTM_TRACE_DEBUG("btm_ble_io_capabilities_req 3: auth_req:%d",
                    p_data->auth_req);
    BTM_TRACE_DEBUG("btm_ble_io_capabilities_req 4: i_keys=0x%x r_keys=0x%x",
                    p_data->init_keys, p_data->resp_keys);

    BTM_TRACE_DEBUG(
        "btm_ble_io_capabilities_req 5: p_data->io_cap = %d auth_req:%d",
        p_data->io_cap, p_data->auth_req);

    /* remove MITM protection requirement if IO cap does not allow it */
    if ((p_data->io_cap == BTM_IO_CAP_NONE) && p_data->oob_data == SMP_OOB_NONE)
      p_data->auth_req &= ~BTM_LE_AUTH_REQ_MITM;

    if (!(p_data->auth_req & SMP_SC_SUPPORT_BIT)) {
      /* if Secure Connections are not supported then remove LK derivation,
      ** and keypress notifications.
      */
      BTM_TRACE_DEBUG(
          "%s-SC not supported -> No LK derivation, no keypress notifications",
          __func__);
      p_data->auth_req &= ~SMP_KP_SUPPORT_BIT;
      p_data->init_keys &= ~SMP_SEC_KEY_TYPE_LK;
      p_data->resp_keys &= ~SMP_SEC_KEY_TYPE_LK;
    }

    BTM_TRACE_DEBUG(
        "btm_ble_io_capabilities_req 6: IO_CAP:%d oob_data:%d auth_req:0x%02x",
        p_data->io_cap, p_data->oob_data, p_data->auth_req);
  }
  return callback_rc;
}

/*******************************************************************************
 *
 * Function         btm_ble_br_keys_req
 *
 * Description      This function is called to handle SMP request for keys sent
 *                  over BR/EDR.
 *
 * Returns          void
 *
 ******************************************************************************/
uint8_t btm_ble_br_keys_req(tBTM_SEC_DEV_REC* p_dev_rec,
                            tBTM_LE_IO_REQ* p_data) {
  uint8_t callback_rc = BTM_SUCCESS;
  BTM_TRACE_DEBUG("%s", __func__);
  if (btm_cb.api.p_le_callback) {
    /* the callback function implementation may change the IO capability... */
    callback_rc = (*btm_cb.api.p_le_callback)(
        BTM_LE_IO_REQ_EVT, p_dev_rec->bd_addr, (tBTM_LE_EVT_DATA*)p_data);
  }

  return callback_rc;
}

/*******************************************************************************
 *
 * Function         btm_ble_connected
 *
 * Description      This function is when a LE connection to the peer device is
 *                  establsihed
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_connected(const RawAddress& bda, uint16_t handle, uint8_t enc_mode,
                       uint8_t role, tBLE_ADDR_TYPE addr_type,
                       UNUSED_ATTR bool addr_matched) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bda);
  tBTM_BLE_CB* p_cb = &btm_cb.ble_ctr_cb;

  BTM_TRACE_EVENT("btm_ble_connected");

  /* Commenting out trace due to obf/compilation problems.
  */
  if (p_dev_rec) {
    VLOG(1) << __func__ << " Security Manager: handle:" << handle
            << " enc_mode:" << loghex(enc_mode) << "  bda: " << bda
            << " RName: " << p_dev_rec->sec_bd_name
            << " p_dev_rec:" << p_dev_rec;

    BTM_TRACE_DEBUG("btm_ble_connected sec_flags=0x%x", p_dev_rec->sec_flags);
  } else {
    VLOG(1) << __func__ << " Security Manager: handle:" << handle
            << " enc_mode:" << loghex(enc_mode) << "  bda: " << bda
            << " p_dev_rec:" << p_dev_rec;
  }

  if (!p_dev_rec) {
    /* There is no device record for new connection.  Allocate one */
    p_dev_rec = btm_sec_alloc_dev(bda);
    if (p_dev_rec == NULL) return;
  } else /* Update the timestamp for this device */
  {
    p_dev_rec->timestamp = btm_cb.dev_rec_count++;
  }

  /* update device information */
  p_dev_rec->device_type |= BT_DEVICE_TYPE_BLE;
  p_dev_rec->ble_hci_handle = handle;
  p_dev_rec->ble.ble_addr_type = addr_type;
  /* update pseudo address */
  p_dev_rec->ble.pseudo_addr = bda;

  p_dev_rec->role_master = false;
  if (role == HCI_ROLE_MASTER) p_dev_rec->role_master = true;

#if (BLE_PRIVACY_SPT == TRUE)
  if (!addr_matched) p_dev_rec->ble.active_addr_type = BTM_BLE_ADDR_PSEUDO;

  if (p_dev_rec->ble.ble_addr_type == BLE_ADDR_RANDOM && !addr_matched)
    p_dev_rec->ble.cur_rand_addr = bda;
#endif

  p_cb->inq_var.directed_conn = BTM_BLE_CONNECT_EVT;

  return;
}

/*****************************************************************************
 *  Function        btm_proc_smp_cback
 *
 *  Description     This function is the SMP callback handler.
 *
 *****************************************************************************/
uint8_t btm_proc_smp_cback(tSMP_EVT event, const RawAddress& bd_addr,
                           tSMP_EVT_DATA* p_data) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  uint8_t res = 0;

  BTM_TRACE_DEBUG("btm_proc_smp_cback event = %d", event);

  if (p_dev_rec != NULL) {
    switch (event) {
      case SMP_IO_CAP_REQ_EVT:
        btm_ble_io_capabilities_req(p_dev_rec,
                                    (tBTM_LE_IO_REQ*)&p_data->io_req);
        break;

      case SMP_BR_KEYS_REQ_EVT:
        btm_ble_br_keys_req(p_dev_rec, (tBTM_LE_IO_REQ*)&p_data->io_req);
        break;

      case SMP_PASSKEY_REQ_EVT:
      case SMP_PASSKEY_NOTIF_EVT:
      case SMP_OOB_REQ_EVT:
      case SMP_NC_REQ_EVT:
      case SMP_SC_OOB_REQ_EVT:
        p_dev_rec->sec_flags |= BTM_SEC_LE_AUTHENTICATED;
        FALLTHROUGH_INTENDED; /* FALLTHROUGH */

      case SMP_CONSENT_REQ_EVT:
      case SMP_SEC_REQUEST_EVT:
        if (event == SMP_SEC_REQUEST_EVT &&
            btm_cb.pairing_state != BTM_PAIR_STATE_IDLE) {
          BTM_TRACE_DEBUG("%s: Ignoring SMP Security request", __func__);
          break;
        }
        btm_cb.pairing_bda = bd_addr;
        if (event != SMP_CONSENT_REQ_EVT) {
          p_dev_rec->sec_state = BTM_SEC_STATE_AUTHENTICATING;
        }
        btm_cb.pairing_flags |= BTM_PAIR_FLAGS_LE_ACTIVE;
        FALLTHROUGH_INTENDED; /* FALLTHROUGH */

      case SMP_COMPLT_EVT:
        if (btm_cb.api.p_le_callback) {
          /* the callback function implementation may change the IO
           * capability... */
          BTM_TRACE_DEBUG("btm_cb.api.p_le_callback=0x%x",
                          btm_cb.api.p_le_callback);
          (*btm_cb.api.p_le_callback)(event, bd_addr,
                                      (tBTM_LE_EVT_DATA*)p_data);
        }

        if (event == SMP_COMPLT_EVT) {
          p_dev_rec = btm_find_dev(bd_addr);
          if (p_dev_rec == NULL) {
            BTM_TRACE_ERROR("%s: p_dev_rec is NULL", __func__);
            android_errorWriteLog(0x534e4554, "120612744");
            return 0;
          }
          BTM_TRACE_DEBUG(
              "evt=SMP_COMPLT_EVT before update sec_level=0x%x sec_flags=0x%x",
              p_data->cmplt.sec_level, p_dev_rec->sec_flags);

          res = (p_data->cmplt.reason == SMP_SUCCESS) ? BTM_SUCCESS
                                                      : BTM_ERR_PROCESSING;
            if (p_dev_rec->sec_smp_pair_pending & BTM_SEC_SMP_PAIR_PENDING) {
              BTM_TRACE_DEBUG("btm_proc_smp_cback - Resetting "
                "Sec_smp_pair_pending = %d", p_dev_rec->sec_smp_pair_pending);
              if (p_dev_rec->sec_smp_pair_pending > BTM_SEC_SMP_PAIR_PENDING) {
                p_dev_rec->link_key_type = (p_dev_rec->sec_smp_pair_pending
                  & BTM_SEC_LINK_KEY_TYPE_UNAUTH) ? BTM_LKEY_TYPE_UNAUTH_COMB
                  : BTM_LKEY_TYPE_AUTH_COMB;
                BTM_TRACE_DEBUG("updated link key type to %d",
                        p_dev_rec->link_key_type);
                btm_send_link_key_notif(p_dev_rec);
              }
              p_dev_rec->sec_smp_pair_pending = BTM_SEC_SMP_NO_PAIR_PENDING;
            }

          BTM_TRACE_DEBUG(
              "after update result=%d sec_level=0x%x sec_flags=0x%x", res,
              p_data->cmplt.sec_level, p_dev_rec->sec_flags);

          if (p_data->cmplt.is_pair_cancel &&
              btm_cb.api.p_bond_cancel_cmpl_callback) {
            BTM_TRACE_DEBUG("Pairing Cancel completed");
            (*btm_cb.api.p_bond_cancel_cmpl_callback)(BTM_SUCCESS);
          }
#if (BTM_BLE_CONFORMANCE_TESTING == TRUE)
          if (res != BTM_SUCCESS) {
            if (!btm_cb.devcb.no_disc_if_pair_fail &&
                p_data->cmplt.reason != SMP_CONN_TOUT) {
              BTM_TRACE_DEBUG("Pairing failed - prepare to remove ACL");
              l2cu_start_post_bond_timer(p_dev_rec->ble_hci_handle);
            } else {
              BTM_TRACE_DEBUG("Pairing failed - Not Removing ACL");
              p_dev_rec->sec_state = BTM_SEC_STATE_IDLE;
            }
          }
#else
          if (res != BTM_SUCCESS && p_data->cmplt.reason != SMP_CONN_TOUT) {
            BTM_TRACE_WARNING("Pairing failed - prepare to remove ACL");
#ifdef ADV_AUDIO_FEATURE
            if (is_remote_support_adv_audio(p_dev_rec->bd_addr)) {
              int status = L2CA_SetFixedChannelTout(p_dev_rec->bd_addr, L2CAP_ATT_CID,
                      L2CAP_BONDING_TIMEOUT * 1000);
              BTM_TRACE_WARNING("%s Setting BONDING timeout for ATT CID status=0x%x",
                  __func__, status);
            }
#endif
            l2cu_start_post_bond_timer(p_dev_rec->ble_hci_handle);
          }
#endif

          BTM_TRACE_DEBUG(
              "btm_cb pairing_state=%x pairing_flags=%x pin_code_len=%x",
              btm_cb.pairing_state, btm_cb.pairing_flags, btm_cb.pin_code_len);
          VLOG(1) << "btm_cb.pairing_bda: " << btm_cb.pairing_bda;

          /* Reset btm state only if the callback address matches pairing
           * address*/
          if (bd_addr == btm_cb.pairing_bda) {
            btm_cb.pairing_bda = RawAddress::kAny;
            btm_cb.pairing_state = BTM_PAIR_STATE_IDLE;
            btm_cb.pairing_flags = 0;
          }

          if (res == BTM_SUCCESS) {
            p_dev_rec->sec_state = BTM_SEC_STATE_IDLE;
#if (BLE_PRIVACY_SPT == TRUE)
            /* add all bonded device into resolving list if IRK is available*/
            btm_ble_resolving_list_load_dev(p_dev_rec);
#endif
          }

          btm_sec_dev_rec_cback_event(p_dev_rec, res, true);
        }
        break;

      default:
        BTM_TRACE_DEBUG("unknown event = %d", event);
        break;
    }
  } else {
    // If we are being paired with via OOB we haven't created a dev rec for
    // the device yet
    if (event == SMP_SC_LOC_OOB_DATA_UP_EVT) {
      btm_sec_cr_loc_oob_data_cback_event(bd_addr, p_data->loc_oob_data);
    } else {
      LOG_WARN(LOG_TAG, "Unexpected event '%d' without p_dev_rec", event);
    }
  }

  return 0;
}

/*******************************************************************************
 *
 * Function         BTM_BleDataSignature
 *
 * Description      This function is called to sign the data using AES128 CMAC
 *                  algorith.
 *
 * Parameter        bd_addr: target device the data to be signed for.
 *                  p_text: singing data
 *                  len: length of the data to be signed.
 *                  signature: output parameter where data signature is going to
 *                             be stored.
 *
 * Returns          true if signing sucessul, otherwise false.
 *
 ******************************************************************************/
bool BTM_BleDataSignature(const RawAddress& bd_addr, uint8_t* p_text,
                          uint16_t len, BLE_SIGNATURE signature) {
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bd_addr);

  BTM_TRACE_DEBUG("%s", __func__);
  if (p_rec == NULL) {
    BTM_TRACE_ERROR("%s-data signing can not be done from unknown device",
                    __func__);
    return false;
  }

  uint8_t* p_mac = (uint8_t*)signature;
  uint8_t* pp;
  uint8_t* p_buf = (uint8_t*)osi_malloc(len + 4);

  BTM_TRACE_DEBUG("%s-Start to generate Local CSRK", __func__);
  pp = p_buf;
  /* prepare plain text */
  if (p_text) {
    memcpy(p_buf, p_text, len);
    pp = (p_buf + len);
  }

  UINT32_TO_STREAM(pp, p_rec->ble.keys.local_counter);
  UINT32_TO_STREAM(p_mac, p_rec->ble.keys.local_counter);

  crypto_toolbox::aes_cmac(p_rec->ble.keys.lcsrk, p_buf, (uint16_t)(len + 4),
                           BTM_CMAC_TLEN_SIZE, p_mac);
  btm_ble_increment_sign_ctr(bd_addr, true);

  BTM_TRACE_DEBUG("%s p_mac = %d", __func__, p_mac);
  BTM_TRACE_DEBUG(
      "p_mac[0] = 0x%02x p_mac[1] = 0x%02x p_mac[2] = 0x%02x p_mac[3] = "
      "0x%02x",
      *p_mac, *(p_mac + 1), *(p_mac + 2), *(p_mac + 3));
  BTM_TRACE_DEBUG(
      "p_mac[4] = 0x%02x p_mac[5] = 0x%02x p_mac[6] = 0x%02x p_mac[7] = "
      "0x%02x",
      *(p_mac + 4), *(p_mac + 5), *(p_mac + 6), *(p_mac + 7));
  osi_free(p_buf);
  return true;
}

/*******************************************************************************
 *
 * Function         BTM_BleVerifySignature
 *
 * Description      This function is called to verify the data signature
 *
 * Parameter        bd_addr: target device the data to be signed for.
 *                  p_orig:  original data before signature.
 *                  len: length of the signing data
 *                  counter: counter used when doing data signing
 *                  p_comp: signature to be compared against.

 * Returns          true if signature verified correctly; otherwise false.
 *
 ******************************************************************************/
bool BTM_BleVerifySignature(const RawAddress& bd_addr, uint8_t* p_orig,
                            uint16_t len, uint32_t counter, uint8_t* p_comp) {
  bool verified = false;
  tBTM_SEC_DEV_REC* p_rec = btm_find_dev(bd_addr);
  uint8_t p_mac[BTM_CMAC_TLEN_SIZE];

  if (p_rec == NULL || (p_rec && !(p_rec->ble.key_type & BTM_LE_KEY_PCSRK))) {
    BTM_TRACE_ERROR("can not verify signature for unknown device");
  } else if (counter < p_rec->ble.keys.counter) {
    BTM_TRACE_ERROR("signature received with out dated sign counter");
  } else if (p_orig == NULL) {
    BTM_TRACE_ERROR("No signature to verify");
  } else {
    BTM_TRACE_DEBUG("%s rcv_cnt=%d >= expected_cnt=%d", __func__, counter,
                    p_rec->ble.keys.counter);

    crypto_toolbox::aes_cmac(p_rec->ble.keys.pcsrk, p_orig, len,
                             BTM_CMAC_TLEN_SIZE, p_mac);
    if (memcmp(p_mac, p_comp, BTM_CMAC_TLEN_SIZE) == 0) {
      btm_ble_increment_sign_ctr(bd_addr, false);
      verified = true;
    }
  }
  return verified;
}

/*******************************************************************************
 *
 * Function         BTM_GetLeSecurityState
 *
 * Description      This function is called to get security mode 1 flags and
 *                  encryption key size for LE peer.
 *
 * Returns          bool    true if LE device is found, false otherwise.
 *
 ******************************************************************************/
bool BTM_GetLeSecurityState(const RawAddress& bd_addr,
                            uint8_t* p_le_dev_sec_flags,
                            uint8_t* p_le_key_size) {
  tBTM_SEC_DEV_REC* p_dev_rec;
  uint16_t dev_rec_sec_flags;

  *p_le_dev_sec_flags = 0;
  *p_le_key_size = 0;

  p_dev_rec = btm_find_dev(bd_addr);
  if (p_dev_rec == NULL) {
    BTM_TRACE_ERROR("%s fails", __func__);
    return (false);
  }

  if (p_dev_rec->ble_hci_handle == BTM_SEC_INVALID_HANDLE) {
    BTM_TRACE_ERROR("%s-this is not LE device", __func__);
    return (false);
  }

  dev_rec_sec_flags = p_dev_rec->sec_flags;

  if (dev_rec_sec_flags & BTM_SEC_LE_ENCRYPTED) {
    /* link is encrypted with LTK or STK */
    *p_le_key_size = p_dev_rec->enc_key_size;
    *p_le_dev_sec_flags |= BTM_SEC_LE_LINK_ENCRYPTED;

    *p_le_dev_sec_flags |=
        (dev_rec_sec_flags & BTM_SEC_LE_AUTHENTICATED)
            ? BTM_SEC_LE_LINK_PAIRED_WITH_MITM     /* set auth LTK flag */
            : BTM_SEC_LE_LINK_PAIRED_WITHOUT_MITM; /* set unauth LTK flag */
  } else if (p_dev_rec->ble.key_type & BTM_LE_KEY_PENC) {
    /* link is unencrypted, still LTK is available */
    *p_le_key_size = p_dev_rec->ble.keys.key_size;

    *p_le_dev_sec_flags |=
        (dev_rec_sec_flags & BTM_SEC_LE_LINK_KEY_AUTHED)
            ? BTM_SEC_LE_LINK_PAIRED_WITH_MITM     /* set auth LTK flag */
            : BTM_SEC_LE_LINK_PAIRED_WITHOUT_MITM; /* set unauth LTK flag */
  }

  BTM_TRACE_DEBUG("%s - le_dev_sec_flags: 0x%02x, le_key_size: %d", __func__,
                  *p_le_dev_sec_flags, *p_le_key_size);

  return true;
}

/*******************************************************************************
 *
 * Function         BTM_BleSecurityProcedureIsRunning
 *
 * Description      This function indicates if LE security procedure is
 *                  currently running with the peer.
 *
 * Returns          bool    true if security procedure is running, false
 *                  otherwise.
 *
 ******************************************************************************/
bool BTM_BleSecurityProcedureIsRunning(const RawAddress& bd_addr) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);

  if (p_dev_rec == NULL) {
    LOG(ERROR) << __func__ << " device with BDA: " << bd_addr
               << " is not found";
    return false;
  }

  return (p_dev_rec->sec_state == BTM_SEC_STATE_ENCRYPTING ||
          p_dev_rec->sec_state == BTM_SEC_STATE_AUTHENTICATING);
}

/*******************************************************************************
 *
 * Function         BTM_BleGetSupportedKeySize
 *
 * Description      This function gets the maximum encryption key size in bytes
 *                  the local device can suport.
 *                  record.
 *
 * Returns          the key size or 0 if the size can't be retrieved.
 *
 ******************************************************************************/
extern uint8_t BTM_BleGetSupportedKeySize(const RawAddress& bd_addr) {
#if (L2CAP_LE_COC_INCLUDED == TRUE)
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bd_addr);
  tBTM_LE_EVT_DATA btm_le_evt_data;
  uint8_t callback_rc;

  if (!p_dev_rec) {
    LOG(ERROR) << __func__ << " device with BDA: " << bd_addr
               << " is not found";
    return 0;
  }

  if (btm_cb.api.p_le_callback == NULL) {
    BTM_TRACE_ERROR("%s can't access supported key size", __func__);
    return 0;
  }

  callback_rc = (*btm_cb.api.p_le_callback)(
      BTM_LE_IO_REQ_EVT, p_dev_rec->bd_addr, &btm_le_evt_data);

  if (callback_rc != BTM_SUCCESS) {
    BTM_TRACE_ERROR("%s can't access supported key size", __func__);
    return 0;
  }

  BTM_TRACE_DEBUG("%s device supports key size = %d", __func__,
                  btm_le_evt_data.io_req.max_key_size);
  return (btm_le_evt_data.io_req.max_key_size);
#else
  return 0;
#endif
}

/*******************************************************************************
 *
 * Function         BTM_GetRemoteQLLFeatures
 *
 * Description      This function is called to get remote QLL features
 *
 * Parameters       features - 8 bytes array for features value
 *
 * Returns          true if feature value is available
 *
 ******************************************************************************/
bool BTM_GetRemoteQLLFeatures(uint16_t handle, uint8_t* features) {
  int idx;
  bool res = false;

  if (!controller_get_interface()->is_qbce_QLE_HCI_supported()) {
    BTM_TRACE_DEBUG("%s: QHS not support", __func__);
    return false;
  }

  idx = btm_handle_to_acl_index(handle);
  if (idx == MAX_L2CAP_LINKS) {
    BTM_TRACE_ERROR("%s: can't find acl for handle: 0x%04x", __func__, handle);
    return false;
  }

  BTM_TRACE_DEBUG("%s: qll_features_state = %x", __func__,
         btm_cb.acl_db[idx].qll_features_state);

  if (btm_cb.acl_db[idx].qll_features_state != BTM_QLL_FEATURES_STATE_FEATURE_COMPLETE) {
    BD_FEATURES value;
    size_t length = sizeof(value);

    if (btif_config_get_bin(btm_cb.acl_db[idx].remote_addr.ToString().c_str(),
                "QLL_FEATURES", value, &length)) {
      BTM_TRACE_DEBUG("%s: reading feature from config file", __func__);
      btm_cb.acl_db[idx].qll_features_state = BTM_QLL_FEATURES_STATE_FEATURE_COMPLETE;
      memcpy(btm_cb.acl_db[idx].remote_qll_features, value, BD_FEATURES_LEN);
      res = true;
    }
  } else {
    res = true;
  }

  if (res && features) {
    memcpy(features, btm_cb.acl_db[idx].remote_qll_features, BD_FEATURES_LEN);
  }
  return res;
}

/*******************************************************************************
 *
 * Function         BTM_QHS_Phy_supported
 *
 * Description      This function is called to determine if QHS phy can be used
 *
 * Parameter        connection handle
 *
 * Returns          bool true if qhs phy can be used
 *
 ******************************************************************************/
bool BTM_QHS_Phy_supported(uint16_t handle) {
  bool qhs_phy = false;
  bool ret;
  BD_FEATURES features;

  ret = BTM_GetRemoteQLLFeatures(handle, (uint8_t *)&features);
  if (ret && (features[2] & 0x40))
    qhs_phy = true;

  return qhs_phy;
}

/*******************************************************************************
 *
 * Function         BTM_BleIsQHSPhySupported
 *
 * Description      This function is called to determine if QHS phy can be used
 *
 * Parameter        bda: BD address of the remote device
 *
 * Returns          bool true if qhs phy can be used, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsQHSPhySupported(const RawAddress& bda) {
  tACL_CONN* p = btm_bda_to_acl(bda, BT_TRANSPORT_LE);
  if (p == NULL) {
    BTM_TRACE_ERROR("%s: invalid bda %s", __func__,
                     bda.ToString().c_str());
    return false;
  }
  return BTM_QHS_Phy_supported(p->hci_handle);
}

/*******************************************************************************
 *
 * Function         BTM_BleIsCisParamUpdateRemoteControllerSupported
 *
 * Description      This function is called to determine if CIS_Parameter_Update_Controller
 *                  feature is supported on remote device.
 *
 * Parameter        bda: BD address of the remote device
 *
 * Returns          bool true if supported, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsCisParamUpdateRemoteControllerSupported(const RawAddress& bda) {
  tACL_CONN* p = btm_bda_to_acl(bda, BT_TRANSPORT_LE);
  if (p == NULL) {
    BTM_TRACE_ERROR("%s: invalid bda %s", __func__,
                     bda.ToString().c_str());
    return false;
  }

  bool supported = false;
  bool ret;
  BD_FEATURES features;

  ret = BTM_GetRemoteQLLFeatures(p->hci_handle, (uint8_t *)&features);
  if (ret && HCI_QBCE_QLL_CIS_PARAMETER_UPDATE_CONTROLLER(features))
    supported = true;

  BTM_TRACE_DEBUG("%s: device=%s, supported=%d",
      __func__, bda.ToString().c_str(), supported);

  return supported;
}

/*******************************************************************************
 *
 * Function         BTM_BleIsCisParamUpdateLocalControllerSupported
 *
 * Description      This function is called to determine if CIS_Parameter_Update_Controller
 *                  feature is supported on local device.
 *
 * Returns          bool true if supported, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsCisParamUpdateLocalControllerSupported() {
  bool supported = false;

  const bt_device_qll_local_supported_features_t *qll_feature_list =
      controller_get_interface()->get_qll_features();
  if (HCI_QBCE_QLL_CIS_PARAMETER_UPDATE_CONTROLLER(qll_feature_list->as_array))
    supported = true;

  BTM_TRACE_DEBUG("%s: supported=%d", __func__, supported);

  return supported;
}

/*******************************************************************************
 *
 * Function         BTM_BleIsCisParamUpdateLocalHostSupported
 *
 * Description      This function is called to determine if CIS_Parameter_Update_Host
 *                  feature is supported by local host.
 *
 * Returns          bool true if supported, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsCisParamUpdateLocalHostSupported() {
  bool supported = false;

  char value[PROPERTY_VALUE_MAX] = "true";
  property_get("persist.vendor.service.bt.cis_param_update_enabled", value, "true");
  if (!strncmp("true", value, 4))
    supported = true;

  BTM_TRACE_DEBUG("%s: supported=%d", __func__, supported);

  return supported;
}

/*******************************************************************************
 *
 * Function         BTM_BleIsCisParamUpdateSupported
 *
 * Description      This function is called to determine if CIS_Parameter_Update
 *                  feature is supported by local controller & local host & remote
 *                  controller.
 *
 * Returns          bool true if supported, false otherwise
 *
 ******************************************************************************/
bool BTM_BleIsCisParamUpdateSupported(const RawAddress& bda) {
  bool supported = BTM_BleIsCisParamUpdateLocalControllerSupported() &&
      BTM_BleIsCisParamUpdateLocalHostSupported() &&
      BTM_BleIsCisParamUpdateRemoteControllerSupported(bda);

  BTM_TRACE_DEBUG("%s: supported=%d", __func__, supported);

  return supported;
}

/*******************************************************************************
 *  Utility functions for LE device IR/ER generation
 ******************************************************************************/
/** This function is to notify application new keys have been generated. */
static void btm_notify_new_key(uint8_t key_type) {
  tBTM_BLE_LOCAL_KEYS* p_local_keys = NULL;

  BTM_TRACE_DEBUG("btm_notify_new_key key_type=%d", key_type);

  if (btm_cb.api.p_le_key_callback) {
    switch (key_type) {
      case BTM_BLE_KEY_TYPE_ID:
        BTM_TRACE_DEBUG("BTM_BLE_KEY_TYPE_ID");
        p_local_keys = (tBTM_BLE_LOCAL_KEYS*)&btm_cb.devcb.id_keys;
        break;

      case BTM_BLE_KEY_TYPE_ER:
        BTM_TRACE_DEBUG("BTM_BLE_KEY_TYPE_ER");
        p_local_keys =
            (tBTM_BLE_LOCAL_KEYS*)&btm_cb.devcb.ble_encryption_key_value;
        break;

      default:
        BTM_TRACE_ERROR("unknown key type: %d", key_type);
        break;
    }
    if (p_local_keys != NULL)
      (*btm_cb.api.p_le_key_callback)(key_type, p_local_keys);
  }
}

/** implementation of btm_ble_reset_id */
static void btm_ble_reset_id_impl(const Octet16& rand1, const Octet16& rand2) {
  /* Regenerate Identity Root */
  btm_cb.devcb.id_keys.ir = rand1;
  uint8_t btm_ble_dhk_pt = 0x03;

  /* generate DHK= Eir({0x03, 0x00, 0x00 ...}) */
  btm_cb.devcb.id_keys.dhk =
      crypto_toolbox::aes_128(btm_cb.devcb.id_keys.ir, &btm_ble_dhk_pt, 1);

  uint8_t btm_ble_irk_pt = 0x01;
  /* IRK = D1(IR, 1) */
  btm_cb.devcb.id_keys.irk =
      crypto_toolbox::aes_128(btm_cb.devcb.id_keys.ir, &btm_ble_irk_pt, 1);

  btm_notify_new_key(BTM_BLE_KEY_TYPE_ID);

#if (BLE_PRIVACY_SPT == TRUE)
  /* if privacy is enabled, new RPA should be calculated */
  if (btm_cb.ble_ctr_cb.privacy_mode != BTM_PRIVACY_NONE) {
    btm_gen_resolvable_private_addr(base::Bind(&btm_gen_resolve_paddr_low));
  }
#endif

  /* proceed generate ER */
  btm_cb.devcb.ble_encryption_key_value = rand2;
  btm_notify_new_key(BTM_BLE_KEY_TYPE_ER);
}

struct reset_id_data {
  Octet16 rand1;
  Octet16 rand2;
};

/** This function is called to reset LE device identity. */
void btm_ble_reset_id(void) {
  BTM_TRACE_DEBUG("btm_ble_reset_id");

  /* In order to reset identity, we need four random numbers. Make four nested
   * calls to generate them first, then proceed to perform the actual reset in
   * btm_ble_reset_id_impl. */
  btsnd_hcic_ble_rand(base::Bind([](BT_OCTET8 rand) {
    reset_id_data tmp;
    memcpy(tmp.rand1.data(), rand, BT_OCTET8_LEN);
    btsnd_hcic_ble_rand(base::Bind(
        [](reset_id_data tmp, BT_OCTET8 rand) {
          memcpy(tmp.rand1.data() + 8, rand, BT_OCTET8_LEN);
          btsnd_hcic_ble_rand(base::Bind(
              [](reset_id_data tmp, BT_OCTET8 rand) {
                memcpy(tmp.rand2.data(), rand, BT_OCTET8_LEN);
                btsnd_hcic_ble_rand(base::Bind(
                    [](reset_id_data tmp, BT_OCTET8 rand) {
                      memcpy(tmp.rand2.data() + 8, rand, BT_OCTET8_LEN);
                      // when all random numbers are ready, do the actual reset.
                      btm_ble_reset_id_impl(tmp.rand1, tmp.rand2);
                    },
                    tmp));
              },
              tmp));
        },
        tmp));
  }));
}

/* This function set a random address to local controller. It also temporarily
 * disable scans and adv before sending the command to the controller. */
void btm_ble_set_random_address(const RawAddress& random_bda) {
  tBTM_LE_RANDOM_CB* p_cb = &btm_cb.ble_ctr_cb.addr_mgnt_cb;
  tBTM_BLE_CB* p_ble_cb = &btm_cb.ble_ctr_cb;
  bool adv_mode = btm_cb.ble_ctr_cb.inq_var.adv_mode;

  BTM_TRACE_DEBUG("%s", __func__);

  if (adv_mode == BTM_BLE_ADV_ENABLE)
    btsnd_hcic_ble_set_adv_enable(BTM_BLE_ADV_DISABLE);
  if (BTM_BLE_IS_SCAN_ACTIVE(p_ble_cb->scan_activity)) btm_ble_stop_scan();
  btm_ble_suspend_bg_conn();

  p_cb->private_addr = random_bda;
  btsnd_hcic_ble_set_random_addr(p_cb->private_addr);

  if (adv_mode == BTM_BLE_ADV_ENABLE)
    btsnd_hcic_ble_set_adv_enable(BTM_BLE_ADV_ENABLE);
  if (BTM_BLE_IS_SCAN_ACTIVE(p_ble_cb->scan_activity)) btm_ble_start_scan();
  btm_ble_resume_bg_conn();
}

#if BTM_BLE_CONFORMANCE_TESTING == TRUE
/*******************************************************************************
 *
 * Function         btm_ble_set_no_disc_if_pair_fail
 *
 * Description      This function indicates whether no disconnect of the ACL
 *                  should be used if pairing failed
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_set_no_disc_if_pair_fail(bool disable_disc) {
  BTM_TRACE_DEBUG("btm_ble_set_disc_enable_if_pair_fail disable_disc=%d",
                  disable_disc);
  btm_cb.devcb.no_disc_if_pair_fail = disable_disc;
}

/*******************************************************************************
 *
 * Function         btm_ble_set_test_mac_value
 *
 * Description      This function set test MAC value
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_set_test_mac_value(bool enable, uint8_t* p_test_mac_val) {
  BTM_TRACE_DEBUG("btm_ble_set_test_mac_value enable=%d", enable);
  btm_cb.devcb.enable_test_mac_val = enable;
  memcpy(btm_cb.devcb.test_mac, p_test_mac_val, BT_OCTET8_LEN);
}

/*******************************************************************************
 *
 * Function         btm_ble_set_test_local_sign_cntr_value
 *
 * Description      This function set test local sign counter value
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_set_test_local_sign_cntr_value(bool enable,
                                            uint32_t test_local_sign_cntr) {
  BTM_TRACE_DEBUG(
      "btm_ble_set_test_local_sign_cntr_value enable=%d local_sign_cntr=%d",
      enable, test_local_sign_cntr);
  btm_cb.devcb.enable_test_local_sign_cntr = enable;
  btm_cb.devcb.test_local_sign_cntr = test_local_sign_cntr;
}

/*******************************************************************************
 *
 * Function         btm_ble_set_keep_rfu_in_auth_req
 *
 * Description      This function indicates if RFU bits have to be kept as is
 *                  (by default they have to be set to 0 by the sender).
 *
 * Returns          void
 *
 ******************************************************************************/
void btm_ble_set_keep_rfu_in_auth_req(bool keep_rfu) {
  BTM_TRACE_DEBUG("btm_ble_set_keep_rfu_in_auth_req keep_rfus=%d", keep_rfu);
  btm_cb.devcb.keep_rfu_in_auth_req = keep_rfu;
}

#endif /* BTM_BLE_CONFORMANCE_TESTING */

/* Checks if this is a CIS Connection handle*/
bool btm_ble_is_cis_handle(uint16_t cis_handle) {
  std::map<uint16_t, uint8_t>::iterator it = cis_map.find(cis_handle);
  if (it != cis_map.end()) {
    return TRUE;
  }
  return FALSE;
}

/* removes CIS connection handle from map*/
void btm_ble_remove_cis_handle(uint16_t cis_handle) {
  std::map<uint16_t, uint8_t>::iterator it = cis_map.find(cis_handle);
  if (it != cis_map.end()) {
    cis_map.erase(it);
  } else {
    BTM_TRACE_WARNING("%s: CIS handle %x is not present", __func__, cis_handle);
  }
}

/* removes all CIS connection handles corresponding to mentioned CIG*/
void btm_ble_remove_cis_handles_for_cig(uint8_t cig_id) {
  BTM_TRACE_API("%s: CIG ID: %x ", __func__, cig_id);
  std::map<uint16_t, uint8_t>::iterator it;
  for (it = cis_map.begin(); it != cis_map.end(); ) {
    if (it->second == cig_id) {
      BTM_TRACE_WARNING("%s: removed CIS Handle %x for cig_id %d", __func__, it->first, cig_id);
      cis_map.erase(it++);
    } else {
      it++;
    }
  }
}

/* -------------------------------------------------------------------------------
 * Bluetooth Spec 5.2 HCI Commands: Parsing Command Complete Events and HCI Events
 *--------------------------------------------------------------------------------*/
void btm_ble_set_cig_param_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_SET_CIG_RET_PARAM ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT8(ret_param.cig_id, param);
    STREAM_TO_UINT8(ret_param.cis_count, param);
    ret_param.conn_handle = (uint16_t*)osi_malloc(ret_param.cis_count * sizeof(uint16_t));
    STREAM_TO_ARRAY(ret_param.conn_handle, param,
                    (uint8_t)(ret_param.cis_count * sizeof(uint16_t)));
  }

  if (hci_cmd_cmpl.set_cig_param) {
    (*hci_cmd_cmpl.set_cig_param) (&ret_param);
  }

  //store cis handles
  if (ret_param.status == HCI_SUCCESS) {
    for(int i = 0; i < ret_param.cis_count; i++) {
      cis_map[ret_param.conn_handle[i]] = ret_param.cig_id;
    }
  }

  // deallocate dynamic memory for connection handles
  osi_free(ret_param.conn_handle);
}

void btm_ble_add_cig_config_cmd_cmpl(
    uint8_t cig_id, uint8_t config_id, uint8_t *param, uint16_t param_len) {
  tBTM_BLE_ADD_CIG_CONFIG_RET_PARAM ret_param = {};
  BTM_TRACE_API("%s: cig_id=%d, config_id=%d, param_len = %d", __func__, cig_id, config_id, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  ret_param.cig_id = cig_id;
  ret_param.config_id = config_id;

  if (hci_cmd_cmpl.add_cig_config_cb) {
    (*hci_cmd_cmpl.add_cig_config_cb) (&ret_param);
  }
}

void btm_ble_setup_iso_datapath_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.setup_iso_datapath) {
    (*hci_cmd_cmpl.setup_iso_datapath) (status, conn_handle);
  }

}

void btm_ble_remove_iso_datapath_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.remove_iso_datapath) {
    (*hci_cmd_cmpl.remove_iso_datapath) (status, conn_handle);
  }

}

void btm_ble_create_cis_status_cb (uint8_t* status, uint16_t len) {
  BTM_TRACE_API("%s: status = %d", __func__, *status);
  std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN>::iterator it;
  tBTM_BLE_CREATE_CIS_CB* create_cis_status_cb = NULL;

  it = pending_cis_map.find(last_pending_cis_handle);
  if (it == pending_cis_map.end()) {
    BTM_TRACE_ERROR("%s: Entry not found for handle = %d",
                    __func__, last_pending_cis_handle);
    return;
  }
  create_cis_status_cb = it->second.create_cis_status_cb;

  // Give callback to uppeer layers
  if (create_cis_status_cb) {
    (*create_cis_status_cb) (*status);
  }

  // remove CIS Connection handles from pending connections
  if (*status != HCI_SUCCESS) {
    // cancel connection timeout alarm for unsuccessful request
    alarm_t* conn_timeout_alarm = it->second.conn_timeout_alarm;
    if (alarm_is_scheduled(conn_timeout_alarm)) {
      alarm_free(conn_timeout_alarm);
    }

    // remove all entries from pending cis connection map
    std::vector<uint16_t> *cis_handles = (std::vector<uint16_t> *)it->second.cis_handles;
    int size = (int)cis_handles->size();
    std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN>::iterator it1;

    for(int i = 0; i < size; i++) {
      it1 = pending_cis_map.find((*cis_handles)[i]);
      if (it1 != pending_cis_map.end()) {
        BTM_TRACE_API("%s: removing pending connection entry for handle : %02x",
                          __func__, (*cis_handles)[i]);
        pending_cis_map.erase(it1);
      }
    }
    delete cis_handles;
    last_pending_cis_handle = 0;
  }
}

/* when CIS connection is not established in 30 sec, send HCI disconnect */
void btm_ble_create_cis_timeout (void* p_data) {
  BTM_TRACE_API("%s: ", __func__);
  std::vector<uint16_t> *cis_handles = (std::vector<uint16_t> *)p_data;
  int size = (int)cis_handles->size();

  for(int i = 0; i < size; i++) {
    std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN>::iterator it
        = pending_cis_map.find((*cis_handles)[i]);
    if (it == pending_cis_map.end()) {
      // This CIS is already established
      continue;
    }
    BTM_TRACE_API("%s: handle = %02x", __func__, (*cis_handles)[i]);

    // send HCI Disconnection to this handle
    BTM_BleIsoCisDisconnect((*cis_handles)[i], HCI_ERR_CONNECTION_TOUT, NULL);

    // send CIS Established cb with connection timeout status
    tBTM_BLE_PENDING_CIS_CONN params = it->second;
    tBTM_BLE_CIS_ESTABLISHED_EVT_PARAM ret_param = {};
    ret_param.status = HCI_ERR_CONNECTION_TOUT;
    ret_param.connection_handle = it->first;
    // erase the pending connection entry from map
    pending_cis_map.erase(it);

    if (params.cis_established_evt_cb) {
      (*params.cis_established_evt_cb)(&ret_param);
    }
  }
  delete cis_handles;
}

void btm_ble_cis_established_evt(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_CIS_ESTABLISHED_EVT_PARAM ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  // TODO: Validate that connection handle is received for any status.
  STREAM_TO_UINT16(ret_param.connection_handle, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_ARRAY(ret_param.cig_sync_delay, param, SYNC_DELAY_LEN);
    STREAM_TO_ARRAY(ret_param.cis_sync_delay, param, SYNC_DELAY_LEN);
    STREAM_TO_ARRAY(ret_param.transport_latency_m_to_s, param, TRANSPORT_LATENCY_LEN);
    STREAM_TO_ARRAY(ret_param.transport_latency_s_to_m, param, TRANSPORT_LATENCY_LEN);
    STREAM_TO_UINT8(ret_param.phy_m_to_s, param);
    STREAM_TO_UINT8(ret_param.phy_s_to_m, param);
    STREAM_TO_UINT8(ret_param.nse, param);
    STREAM_TO_UINT8(ret_param.bn_m_to_s, param);
    STREAM_TO_UINT8(ret_param.bn_s_to_m, param);
    STREAM_TO_UINT8(ret_param.ft_m_to_s, param);
    STREAM_TO_UINT8(ret_param.ft_s_to_m, param);
    STREAM_TO_UINT16(ret_param.max_pdu_m_to_s, param);
    STREAM_TO_UINT16(ret_param.max_pdu_s_to_m, param);
    STREAM_TO_UINT16(ret_param.iso_interval, param);
  }

  std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN>::iterator it
      = pending_cis_map.find(ret_param.connection_handle);
  if (it == pending_cis_map.end()) {
    BTM_TRACE_API("%s: CIS for connection handle (%x) is not pending",
        __func__, ret_param.connection_handle);
    return;
  }

  alarm_t* conn_timeout_alarm = it->second.conn_timeout_alarm;
  tBTM_BLE_CIS_ESTABLISHED_CB* cback = it->second.cis_established_evt_cb;

  // cancel the alarm if CIS established event is received for all handles
  std::vector<uint16_t> *cis_handles = (std::vector<uint16_t> *)it->second.cis_handles;
  int size = (int)cis_handles->size();
  uint8_t pending_cis_count  = 0 ;
  std::map<uint16_t, tBTM_BLE_PENDING_CIS_CONN>::iterator it1;
  for(int i = 0; i < size; i++) {
    it1 = pending_cis_map.find((*cis_handles)[i]);
    if (it1 == pending_cis_map.end()) {
      // This CIS is already established
      continue;
    }
    pending_cis_count++;
  }

  if (pending_cis_count <= 1) {
    if (alarm_is_scheduled(conn_timeout_alarm)) {
      BTM_TRACE_DEBUG("%s last pending CIS established, free alarm", __func__);
      alarm_free(conn_timeout_alarm);
    }
  }

  // remove the entry from map
  pending_cis_map.erase(it);

  // Give callback to upper layer
  if (cback) {
    (*cback) (&ret_param);
  }
}

void btm_ble_cis_request_evt(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_CIS_REQ_EVT_PARAM evt_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT16(evt_param.acl_conn_handle, param);
  STREAM_TO_UINT16(evt_param.cis_conn_handle, param);
  STREAM_TO_UINT8(evt_param.cig_id, param);
  STREAM_TO_UINT8(evt_param.cis_id, param);

  if (hci_cmd_cmpl.cis_request_evt_cb) {
    (*hci_cmd_cmpl.cis_request_evt_cb) (&evt_param);
  }

  // store cis handle
  cis_map[evt_param.cis_conn_handle] = evt_param.cig_id;
}

void btm_ble_remove_cig_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint8_t cig_id = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  if (status == HCI_SUCCESS) {
    STREAM_TO_UINT8(cig_id, param);
  }
  if (hci_cmd_cmpl.remove_cig) {
    (*hci_cmd_cmpl.remove_cig) (status, cig_id);
  }

  // clear all cis handles corresponding to cig_id
  if (status == HCI_SUCCESS) {
    btm_ble_remove_cis_handles_for_cig(cig_id);
  }
}

void btm_ble_reject_cis_req_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.reject_cis_cb) {
    (*hci_cmd_cmpl.reject_cis_cb) (status, conn_handle);
  }

}

void btm_ble_peer_sca_cmpl_evt(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_PEER_SCA_PARAM evt_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(evt_param.status, param);
  // TODO: to validate if below parameters are received for all result codes
  STREAM_TO_UINT16(evt_param.conn_handle, param);
  STREAM_TO_UINT8(evt_param.sca, param);

  if (hci_cmd_cmpl.peer_sca_cmpl ) {
    (*hci_cmd_cmpl.peer_sca_cmpl ) (&evt_param);
  }
}

void btm_ble_read_iso_link_qlt_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_ISO_LINK_QLT ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  STREAM_TO_UINT16(ret_param.conn_handle, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT32(ret_param.tx_unacked_packets, param);
    STREAM_TO_UINT32(ret_param.tx_flushed_packets, param);
    STREAM_TO_UINT32(ret_param.tx_last_subevent_packets, param);
    STREAM_TO_UINT32(ret_param.retransmitted_packets, param);
    STREAM_TO_UINT32(ret_param.crc_error_packets, param);
    STREAM_TO_UINT32(ret_param.rx_unreceived_packets, param);
    STREAM_TO_UINT32(ret_param.duplicate_packets, param);
  }

  if (hci_cmd_cmpl.iso_link_qly_cb) {
    (*hci_cmd_cmpl.iso_link_qly_cb) (&ret_param);
  }

}

void btm_ble_enh_read_tx_pow_level_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_ENH_READ_TX_POWER_LEVEL ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  STREAM_TO_UINT16(ret_param.conn_handle, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT8(ret_param.phy, param);
    STREAM_TO_UINT8(ret_param.current_transmit_power_level, param);
    STREAM_TO_UINT8(ret_param.max_transmit_power_level, param);
  }

  if (hci_cmd_cmpl.tx_pow_level_cb) {
    (*hci_cmd_cmpl.tx_pow_level_cb) (&ret_param);
  }

}

void btm_ble_read_iso_tx_sync_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_READ_ISO_TX_SYNC_PARAM ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  STREAM_TO_UINT16(ret_param.conn_handle, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT16(ret_param.packet_sequence_number, param);
    STREAM_TO_UINT32(ret_param.time_stamp, param);
    STREAM_TO_ARRAY(ret_param.time_offset, param, TIME_OFFSET_LEN);
  }

  if (hci_cmd_cmpl.iso_tx_sync_cb) {
    (*hci_cmd_cmpl.iso_tx_sync_cb) (&ret_param);
  }

}

void btm_ble_read_remote_tx_pow_level_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_READ_REMOTE_TX_POWER_LEVEL ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  STREAM_TO_UINT16(ret_param.conn_handle, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT8(ret_param.phy, param);
    STREAM_TO_UINT8(ret_param.current_transmit_power_level, param);
    STREAM_TO_UINT8(ret_param.max_transmit_power_level, param);
    STREAM_TO_UINT8(ret_param.delta, param);
  }

  if (hci_cmd_cmpl.remote_tx_pow_cb) {
    (*hci_cmd_cmpl.remote_tx_pow_cb) (&ret_param);
  }

}

void btm_ble_set_pathloss_param_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.pathloss_rpt_cb) {
    (*hci_cmd_cmpl.pathloss_rpt_cb) (status, conn_handle);
  }

}

void btm_ble_set_pathloss_rpt_enable_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.pathloss_rpt_enable_cb) {
    (*hci_cmd_cmpl.pathloss_rpt_enable_cb) (status, conn_handle);
  }

}

void btm_ble_path_loss_threshold_evt(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_PATHLOSS_THRESHOLD_RET evt_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT16(evt_param.conn_handle, param);
  STREAM_TO_UINT8(evt_param.current_path_loss, param);
  STREAM_TO_UINT8(evt_param.zone_entered, param);

  if (hci_cmd_cmpl.pathloss_threshold_evt_cb) {
    (*hci_cmd_cmpl.pathloss_threshold_evt_cb) (&evt_param);
  }
}

void btm_ble_set_tx_pow_rpt_enable_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  STREAM_TO_UINT16(conn_handle, param);

  if (hci_cmd_cmpl.tx_pow_rpt_enable_cb) {
    (*hci_cmd_cmpl.tx_pow_rpt_enable_cb) (status, conn_handle);
  }

}

void btm_ble_transmit_power_reporting_event(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_TX_POW_EVT_PARAM evt_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(evt_param.status, param);
  STREAM_TO_UINT16(evt_param.conn_handle, param);
  STREAM_TO_UINT8(evt_param.reason, param);
  STREAM_TO_UINT8(evt_param.phy, param);
  STREAM_TO_UINT8(evt_param.tx_pow_level, param);
  STREAM_TO_UINT8(evt_param.tx_pow_level_flag, param);
  STREAM_TO_UINT8(evt_param.delta, param);

  if (hci_cmd_cmpl.tx_pow_rpt_evt_cb) {
    (*hci_cmd_cmpl.tx_pow_rpt_evt_cb) (&evt_param);
  }
}

void btm_ble_set_cig_param_test_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_SET_CIG_PARAM_TEST_RET ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT8(ret_param.cig_id, param);
    STREAM_TO_UINT8(ret_param.cis_count, param);
    ret_param.conn_handle = (uint16_t*)osi_malloc(ret_param.cis_count * sizeof(uint16_t));
    STREAM_TO_ARRAY(ret_param.conn_handle, param,
                    (uint8_t)(ret_param.cis_count * sizeof(uint16_t)));
  }

  if (hci_cmd_cmpl.cig_param_test_cmpl) {
    (*hci_cmd_cmpl.cig_param_test_cmpl) (&ret_param);
  }

  if (ret_param.status == HCI_SUCCESS) {
    for (int i = 0; i < ret_param.cis_count; i++) {
      cis_map[ret_param.conn_handle[i]] = ret_param.cig_id;
    }
  }
  // free memory conn_handle memory
  osi_free(ret_param.conn_handle);
}

void btm_ble_iso_test_end_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_ISO_TEST_END_RET ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT16(ret_param.conn_handle, param);
    STREAM_TO_UINT32(ret_param.received_packet_count, param);
    STREAM_TO_UINT32(ret_param.missed_packet_count, param);
    STREAM_TO_UINT32(ret_param.failed_packet_count, param);
  }

  if (hci_cmd_cmpl.iso_test_end_cmpl) {
    (*hci_cmd_cmpl.iso_test_end_cmpl) (&ret_param);
  }

}

void btm_ble_iso_read_test_cnt_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  tBTM_BLE_ISO_TEST_COUNTERS_RET ret_param = {};
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(ret_param.status, param);
  if (ret_param.status == HCI_SUCCESS) {
    STREAM_TO_UINT16(ret_param.conn_handle, param);
    STREAM_TO_UINT32(ret_param.received_packet_count, param);
    STREAM_TO_UINT32(ret_param.missed_packet_count, param);
    STREAM_TO_UINT32(ret_param.failed_packet_count, param);
  }

  if (hci_cmd_cmpl.read_test_cnt_cmpl) {
    (*hci_cmd_cmpl.read_test_cnt_cmpl) (&ret_param);
  }

}

void btm_ble_iso_receive_test_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  if (status == HCI_SUCCESS) {
    STREAM_TO_UINT16(conn_handle, param);
  }
  if (hci_cmd_cmpl.iso_rcv_test_cmpl) {
    (*hci_cmd_cmpl.iso_rcv_test_cmpl) (status, conn_handle);
  }

}

void btm_ble_iso_transmit_test_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  uint16_t conn_handle = 0;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);
  if (status == HCI_SUCCESS) {
    STREAM_TO_UINT16(conn_handle, param);
  }
  if (hci_cmd_cmpl.iso_tx_test_cmpl) {
    (*hci_cmd_cmpl.iso_tx_test_cmpl) (status, conn_handle);
  }

}

void btm_ble_transmitter_testv4_cmd_cmpl(uint8_t *param, uint16_t param_len) {
  uint8_t status;
  BTM_TRACE_API("%s: param_len = %d", __func__, param_len);

  if (param_len <= 0) {
    BTM_TRACE_WARNING("%s Insufficient return parameters.", __func__);
    return;
  }

  STREAM_TO_UINT8(status, param);

  if (hci_cmd_cmpl.tx_test_v4_cmpl) {
    (*hci_cmd_cmpl.tx_test_v4_cmpl) (status);
  }

}

/* ------------------------------------------------------------------------------
 *           Bluetooth Spec 5.2 HCI Commands API Implementation
 *-------------------------------------------------------------------------------*/
uint8_t BTM_BleSetCigParam(tBTM_BLE_ISO_SET_CIG_CMD_PARAM* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
        && !controller_get_interface()->is_cis_master_role_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.set_cig_param = p_data->p_cb;
  btsnd_hcic_ble_set_cig_param(p_data->cig_id,
                               p_data->sdu_int_m_to_s,
                               p_data->sdu_int_s_to_m,
                               p_data->slave_clock_accuracy,
                               p_data->packing,
                               p_data->framing,
                               p_data->max_transport_latency_m_to_s,
                               p_data->max_transport_latency_s_to_m,
                               p_data->cis_count,
                               p_data->cis_config,
                               base::Bind(&btm_ble_set_cig_param_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleAddCigConfig(tBTM_BLE_ISO_ADD_CIG_CONFIG_CMD_PARAM* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
        && !controller_get_interface()->is_cis_master_role_supported()
        && !BTM_BleIsCisParamUpdateLocalControllerSupported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.add_cig_config_cb = p_data->p_cb;
  btsnd_hcic_ble_add_cig_config(p_data->cig_id,
                               p_data->config_id,
                               p_data->mode_id,
                               p_data->sdu_int_m_to_s,
                               p_data->sdu_int_s_to_m,
                               p_data->slave_clock_accuracy,
                               p_data->packing,
                               p_data->framing,
                               p_data->max_transport_latency_m_to_s,
                               p_data->max_transport_latency_s_to_m,
                               p_data->cis_count,
                               p_data->cis_config,
                               base::Bind(&btm_ble_add_cig_config_cmd_cmpl, p_data->cig_id, p_data->config_id));
  return HCI_SUCCESS;
}

uint8_t BTM_BleCreateCis(tBTM_BLE_ISO_CREATE_CIS_CMD_PARAM* p_data,
                         tBTM_BLE_CIS_DISCONNECTED_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
        && !controller_get_interface()->is_cis_master_role_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.cis_disconnected_cb = p_cb;

  /* New Logic to start alarm and preserve handles */
  tBTM_BLE_PENDING_CIS_CONN pending_conn;
  int size = (int)p_data->link_conn_handles.size();

  if (size == 0) {
    BTM_TRACE_ERROR("%s: No connection handles provided", __func__);
    return HCI_ERR_ILLEGAL_COMMAND;
  }

  btsnd_hcic_ble_create_cis(p_data->cis_count,
                            p_data->link_conn_handles,
                            base::Bind(&btm_ble_create_cis_status_cb));

  last_pending_cis_handle = p_data->link_conn_handles[0].cis_conn_handle;
  std::vector<uint16_t> *cis_handles = new std::vector<uint16_t>(size);

  for(int i = 0; i < size; i++) {
    (*cis_handles)[i] = p_data->link_conn_handles[i].cis_conn_handle;
  }

  pending_conn.cis_handles = cis_handles;
  pending_conn.create_cis_status_cb = p_data->p_cb;
  pending_conn.cis_established_evt_cb = p_data->p_evt_cb;
  alarm_t *conn_timeout_alarm = alarm_new("cis_timeout_alarm");
  pending_conn.conn_timeout_alarm = conn_timeout_alarm;
  alarm_set_on_mloop(pending_conn.conn_timeout_alarm, HCI_CIS_CONNECTION_TIMEOUT,
                     btm_ble_create_cis_timeout, cis_handles);

  for(int i = 0; i < size; i++) {
    BTM_TRACE_API("%s: inserting %02x handle into the pending connection map",
                      __func__, (*cis_handles)[i]);
    pending_cis_map[(*cis_handles)[i]] = pending_conn;
  }

  return HCI_SUCCESS;
}

uint8_t BTM_BleRemoveCig(uint8_t cig_id, tBTM_BLE_REMOVE_CIG_CMPL_CB *p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
        && !controller_get_interface()->is_cis_master_role_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.remove_cig = p_cb;
  btsnd_hcic_ble_remove_cig(cig_id,
                            base::Bind(&btm_ble_remove_cig_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleSetIsoDataPath(tBTM_BLE_SET_ISO_DATA_PATH_PARAM* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_cis_master_role_supported() &&
      !controller_get_interface()->supports_ble_iso_broadcaster()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.setup_iso_datapath = p_data->p_cb;
  btsnd_hcic_ble_set_iso_data_path(p_data->conn_handle,
                                   p_data->data_path_dir,
                                   p_data->data_path_id,
                                   p_data->codec_id,
                                   p_data->cont_delay,
                                   p_data->codec_config_length,
                                   p_data->codec_config,
                                   base::Bind(&btm_ble_setup_iso_datapath_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleRemoveIsoDataPath(uint16_t conn_handle, uint8_t direction,
                                 tBTM_BLE_REMOVE_ISO_DATA_PATH_CMPL_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_cis_master_role_supported() &&
      !controller_get_interface()->supports_ble_iso_broadcaster()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.remove_iso_datapath = p_cb;
  btsnd_hcic_ble_remove_iso_data_path(conn_handle, direction,
                                      base::Bind(&btm_ble_remove_iso_datapath_cmd_cmpl));
  return HCI_SUCCESS;
}

void BTM_BleRequestPeerSca(uint16_t conn_handle,
                           tBTM_BLE_REQUEST_PEER_SCA_COMPLETE_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  hci_cmd_cmpl.peer_sca_cmpl = p_cb;
  btsnd_hcic_ble_req_peer_sca(conn_handle);
}

uint8_t BTM_BleReadIsoLinkQuality(uint16_t conn_handle,
                                  tBTM_BLE_READ_ISO_LINK_QLT_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

    if (!controller_get_interface()->is_host_iso_channel_supported() &&
        !(controller_get_interface()->is_cis_master_role_supported()
            || controller_get_interface()->is_cis_slave_role_supported())) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.iso_link_qly_cb = p_cb;
  btsnd_hcic_ble_read_iso_link_quality(conn_handle,
                                       base::Bind(&btm_ble_read_iso_link_qlt_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleReadIsoTxSync (uint16_t conn_handle,
                               tBTM_BLE_READ_ISO_TX_SYNC_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

    if (!controller_get_interface()->is_host_iso_channel_supported() &&
        !(controller_get_interface()->is_cis_master_role_supported()
            || controller_get_interface()->is_cis_slave_role_supported())) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.iso_tx_sync_cb = p_cb;
  btsnd_hcic_ble_read_iso_tx_sync(conn_handle,
                                  base::Bind(&btm_ble_read_iso_tx_sync_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleAcceptCisRequest (uint16_t conn_handle,
                                 tBTM_BLE_CIS_ESTABLISHED_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
          && !controller_get_interface()->is_cis_slave_role_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.cis_established_evt_cb = p_cb;
  btsnd_hcic_ble_accept_cis(conn_handle);
  return HCI_SUCCESS;
}

uint8_t BTM_BleRejectCisRequest (uint16_t conn_handle, uint8_t reason,
                                 tBTM_BLE_REJECT_CIS_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()
          && !controller_get_interface()->is_cis_slave_role_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.reject_cis_cb = p_cb;
  btsnd_hcic_ble_reject_cis(conn_handle, reason,
                            base::Bind(&btm_ble_reject_cis_req_cmd_cmpl));
  return HCI_SUCCESS;
}

void BTM_BleEnhancedReadTransmitPowerLevel (uint16_t conn_handle, uint8_t phy,
                                 tBTM_BLE_ENHANCED_READ_TRANSMIT_POWER_LEVEL_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  hci_cmd_cmpl.tx_pow_level_cb = p_cb;
  btsnd_hcic_ble_read_enhanced_pow_level(conn_handle, phy,
                                         base::Bind(&btm_ble_enh_read_tx_pow_level_cmd_cmpl));
}

uint8_t BTM_BleReadRemoteTransmitPowerLevel(uint16_t conn_handle, uint8_t phy,
                                 tBTM_BLE_READ_REMOTE_TX_POW_LEVEL_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_pow_ctr_req_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.remote_tx_pow_cb = p_cb;
  btsnd_hcic_ble_read_remote_tx_pow_level(conn_handle, phy);
  return HCI_SUCCESS;
}

uint8_t BTM_BleSetPathLossReportingParams(tBTM_BLE_SET_PATH_LOSS_REPORTING_PARAM* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_pathloss_monitoring_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.pathloss_rpt_cb = p_data->p_cb;
  btsnd_hcic_ble_set_path_loss_prt_param(p_data->conn_handle,
                                         p_data->high_threshold,
                                         p_data->high_hysteresis,
                                         p_data->low_threshold,
                                         p_data->low_hysteresis,
                                         p_data->min_time_spent,
                                         base::Bind(&btm_ble_set_pathloss_param_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleSetPathLossReportingEnable (uint16_t conn_handle,
                                           uint8_t enable,
                                           tBTM_BLE_SET_PATH_LOSS_REPORTING_ENABLE_CB* p_cb,
                                           tBTM_BLE_PATH_LOSS_THRESHOLD_CB* p_event_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_pathloss_monitoring_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.pathloss_rpt_enable_cb = p_cb;
  hci_cmd_cmpl.pathloss_threshold_evt_cb = p_event_cb;
  btsnd_hcic_ble_set_path_loss_rpt_enable(conn_handle, enable,
                                          base::Bind(&btm_ble_set_pathloss_rpt_enable_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleSetTransmitPowerReportingEnable(
                                uint16_t conn_handle,
                                uint8_t local_enable,
                                uint8_t remote_enable,
                                tBTM_BLE_SET_TRANSMIT_POWER_REPORTING_ENABLE_CB* p_cmd_cmpl,
                                tBTM_BLE_TRANSMIT_POWER_REPORTING_EVENT_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_pow_ctr_req_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.tx_pow_rpt_enable_cb = p_cmd_cmpl;
  hci_cmd_cmpl.tx_pow_rpt_evt_cb = p_cb;
  btsnd_hcic_ble_set_tx_pow_rpt_enable(conn_handle, local_enable, remote_enable,
                                       base::Bind(&btm_ble_set_tx_pow_rpt_enable_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleIsoTestEnd (uint16_t conn_handle, tBTM_BLE_ISO_TEST_END_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.iso_test_end_cmpl = p_cb;
  btsnd_hcic_ble_iso_test_end(conn_handle,
                              base::Bind(&btm_ble_iso_test_end_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleSetCigParametersTest(tBTM_BLE_SET_CIG_PARAM_TEST* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.cig_param_test_cmpl = p_data->p_cb;
  btsnd_hcic_ble_set_cig_param_test(p_data->cig_id,
                                    p_data->sdu_int_m_to_s,
                                    p_data->sdu_int_s_to_m,
                                    p_data->ft_m_to_s,
                                    p_data->ft_s_to_m,
                                    p_data->iso_interval,
                                    p_data->slave_clock_accuracy,
                                    p_data->packing,
                                    p_data->framing,
                                    p_data->cis_count,
                                    p_data->cis_config,
                                    base::Bind(&btm_ble_set_cig_param_test_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleIsoReadTestCounters(uint16_t conn_handle,
                                    tBTM_BLE_ISO_READ_TEST_COUNTERS_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.read_test_cnt_cmpl = p_cb;
  btsnd_hcic_ble_iso_read_test_counters(conn_handle,
                                        base::Bind(&btm_ble_iso_read_test_cnt_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleIsoReceiveTest(uint16_t conn_handle,
                              uint8_t payload_type, tBTM_BLE_ISO_RECEIVE_TEST_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.iso_rcv_test_cmpl = p_cb;
  btsnd_hcic_ble_iso_receive_test(conn_handle, payload_type,
                                  base::Bind(&btm_ble_iso_receive_test_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleIsoTransmitTest(uint16_t conn_handle,
                               uint8_t payload_type, tBTM_BLE_ISO_TRANSMIT_TEST_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.iso_tx_test_cmpl = p_cb;
  btsnd_hcic_ble_iso_transmit_test(conn_handle, payload_type,
                                  base::Bind(&btm_ble_iso_transmit_test_cmd_cmpl));
  return HCI_SUCCESS;
}

uint8_t BTM_BleTransmitterTestV4(tBTM_BLE_TRANSMITTER_TEST_PARAM* p_data) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.tx_test_v4_cmpl = p_data->p_cb;
  btsnd_hcic_ble_transmitter_test_v4(p_data->tx_channel,
                                     p_data->test_data_length,
                                     p_data->packet_payload,
                                     p_data->phy,
                                     p_data->cte_length,
                                     p_data->cte_type,
                                     p_data->switching_pattern_length,
                                     p_data->antenna_ids,
                                     p_data->transmit_power_level,
                                     base::Bind(&btm_ble_transmitter_testv4_cmd_cmpl));
  return HCI_SUCCESS;
}

void btm_ble_set_cis_req_evt_cb(tBTM_BLE_CIS_REQ_EVT_CB* p_cb) {
  hci_cmd_cmpl.cis_request_evt_cb = p_cb;
}

uint8_t BTM_BleIsoCisDisconnect(uint16_t conn_handle, uint8_t reason,
                                tBTM_BLE_CIS_DISCONNECTED_CB* p_cb) {
  BTM_TRACE_API("%s", __func__);

  if (!controller_get_interface()->is_host_iso_channel_supported()) {
    BTM_TRACE_ERROR("%s: Unsupported feature. Return.", __func__);
    return HCI_ERR_UNSUPPORTED_VALUE;
  }

  hci_cmd_cmpl.cis_disconnected_cb = p_cb;
  btsnd_hcic_disconnect(conn_handle, reason);
  return HCI_SUCCESS;
}

void btm_ble_cis_disconnected(uint8_t status, uint16_t cis_handle, uint8_t reason) {
  BTM_TRACE_WARNING("%s", __func__);

  if (hci_cmd_cmpl.cis_disconnected_cb) {
    (*hci_cmd_cmpl.cis_disconnected_cb)(status, cis_handle, reason);
  }
}

void btm_ble_qle_cis_configuration_state_event(const uint8_t *p) {
  uint8_t cig_id, cis_id, config_id;
  uint8_t status;

  STREAM_TO_UINT8(cig_id, p);
  STREAM_TO_UINT8(cis_id, p);
  STREAM_TO_UINT8(config_id, p);
  STREAM_TO_UINT8(status, p);

  BTM_TRACE_DEBUG("%s: cig_id=%d, cis_id=%d, config_id=%d, status=%d",
      __func__, cig_id, cis_id, config_id, status);
}

void btm_ble_qle_cis_updated_event(const uint8_t *p) {
  uint16_t cis_handle;
  uint8_t config_id;

  STREAM_TO_UINT16(cis_handle, p);
  STREAM_TO_UINT8(config_id, p);

  BTM_TRACE_DEBUG("%s: cis_handle=%d, config_id=%d",
      __func__, cis_handle, config_id);
}

