// Use of this code is governed by a BSD-style license that can be found in the LICENSE file.
/*
 * Copyright(c) 2021 Intel Corporation
 */

#include "iwl-drv.h"
#include "pnvm.h"
#include "iwl-prph.h"
#include "iwl-io.h"

#include "fw/uefi.h"
#include "fw/api/alive.h"
#include <linux/efi.h>
#include "fw/runtime.h"

#define IWL_EFI_VAR_GUID EFI_GUID(0x92daaf2f, 0xc02b, 0x455b,	\
				  0xb2, 0xec, 0xf5, 0xa3,	\
				  0x59, 0x4f, 0x4a, 0xea)

void *iwl_uefi_get_pnvm(struct iwl_trans *trans, size_t *len)
{
	struct efivar_entry *pnvm_efivar;
	void *data;
	unsigned long package_size;
	int err;

	*len = 0;

	pnvm_efivar = kzalloc(sizeof(*pnvm_efivar), GFP_KERNEL);
	if (!pnvm_efivar)
		return ERR_PTR(-ENOMEM);

	memcpy(&pnvm_efivar->var.VariableName, IWL_UEFI_OEM_PNVM_NAME,
	       sizeof(IWL_UEFI_OEM_PNVM_NAME));
	pnvm_efivar->var.VendorGuid = IWL_EFI_VAR_GUID;

	/*
	 * TODO: we hardcode a maximum length here, because reading
	 * from the UEFI is not working.  To implement this properly,
	 * we have to call efivar_entry_size().
	 */
	package_size = IWL_HARDCODED_PNVM_SIZE;

	data = kmalloc(package_size, GFP_KERNEL);
	if (!data) {
		data = ERR_PTR(-ENOMEM);
		goto out;
	}

	err = efivar_entry_get(pnvm_efivar, NULL, &package_size, data);
	if (err) {
		IWL_DEBUG_FW(trans,
			     "PNVM UEFI variable not found %d (len %lu)\n",
			     err, package_size);
		kfree(data);
		data = ERR_PTR(err);
		goto out;
	}

	IWL_DEBUG_FW(trans, "Read PNVM from UEFI with size %lu\n", package_size);
	*len = package_size;

out:
	kfree(pnvm_efivar);

	return data;
}

static void *iwl_uefi_reduce_power_section(struct iwl_trans *trans,
					   const u8 *data, size_t len)
{
	const struct iwl_ucode_tlv *tlv;
	u8 *reduce_power_data = NULL, *tmp;
	u32 size = 0;

	IWL_DEBUG_FW(trans, "Handling REDUCE_POWER section\n");

	while (len >= sizeof(*tlv)) {
		u32 tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);

		if (len < tlv_len) {
			IWL_ERR(trans, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			kfree(reduce_power_data);
			reduce_power_data = ERR_PTR(-EINVAL);
			goto out;
		}

		data += sizeof(*tlv);

		switch (tlv_type) {
		case IWL_UCODE_TLV_MEM_DESC: {
			IWL_DEBUG_FW(trans,
				     "Got IWL_UCODE_TLV_MEM_DESC len %d\n",
				     tlv_len);

			IWL_DEBUG_FW(trans, "Adding data (size %d)\n", tlv_len);

			tmp = krealloc(reduce_power_data, size + tlv_len, GFP_KERNEL);
			if (!tmp) {
				IWL_DEBUG_FW(trans,
					     "Couldn't allocate (more) reduce_power_data\n");

				kfree(reduce_power_data);
				reduce_power_data = ERR_PTR(-ENOMEM);
				goto out;
			}

			reduce_power_data = tmp;

			memcpy(reduce_power_data + size, data, tlv_len);

			size += tlv_len;

			break;
		}
		case IWL_UCODE_TLV_PNVM_SKU:
			IWL_DEBUG_FW(trans,
				     "New REDUCE_POWER section started, stop parsing.\n");
			goto done;
		default:
			IWL_DEBUG_FW(trans, "Found TLV 0x%0x, len %d\n",
				     tlv_type, tlv_len);
			break;
		}

		len -= ALIGN(tlv_len, 4);
		data += ALIGN(tlv_len, 4);
	}

done:
	if (!size) {
		IWL_DEBUG_FW(trans, "Empty REDUCE_POWER, skipping.\n");
		/* Better safe than sorry, but 'reduce_power_data' should
		 * always be NULL if !size.
		 */
		kfree(reduce_power_data);
		reduce_power_data = ERR_PTR(-ENOENT);
		goto out;
	}

	IWL_INFO(trans, "loaded REDUCE_POWER\n");

out:
	return reduce_power_data;
}

static void *iwl_uefi_reduce_power_parse(struct iwl_trans *trans,
					 const u8 *data, size_t len)
{
	const struct iwl_ucode_tlv *tlv;
	void *sec_data;

	IWL_DEBUG_FW(trans, "Parsing REDUCE_POWER data\n");

	while (len >= sizeof(*tlv)) {
		u32 tlv_len, tlv_type;

		len -= sizeof(*tlv);
		tlv = (const void *)data;

		tlv_len = le32_to_cpu(tlv->length);
		tlv_type = le32_to_cpu(tlv->type);

		if (len < tlv_len) {
			IWL_ERR(trans, "invalid TLV len: %zd/%u\n",
				len, tlv_len);
			return ERR_PTR(-EINVAL);
		}

		if (tlv_type == IWL_UCODE_TLV_PNVM_SKU) {
			const struct iwl_sku_id *sku_id =
				(const void *)(data + sizeof(*tlv));

			IWL_DEBUG_FW(trans,
				     "Got IWL_UCODE_TLV_PNVM_SKU len %d\n",
				     tlv_len);
			IWL_DEBUG_FW(trans, "sku_id 0x%0x 0x%0x 0x%0x\n",
				     le32_to_cpu(sku_id->data[0]),
				     le32_to_cpu(sku_id->data[1]),
				     le32_to_cpu(sku_id->data[2]));

			data += sizeof(*tlv) + ALIGN(tlv_len, 4);
			len -= ALIGN(tlv_len, 4);

			if (trans->sku_id[0] == le32_to_cpu(sku_id->data[0]) &&
			    trans->sku_id[1] == le32_to_cpu(sku_id->data[1]) &&
			    trans->sku_id[2] == le32_to_cpu(sku_id->data[2])) {
				sec_data = iwl_uefi_reduce_power_section(trans,
									 data,
									 len);
				if (!IS_ERR(sec_data))
					return sec_data;
			} else {
				IWL_DEBUG_FW(trans, "SKU ID didn't match!\n");
			}
		} else {
			data += sizeof(*tlv) + ALIGN(tlv_len, 4);
			len -= ALIGN(tlv_len, 4);
		}
	}

	return ERR_PTR(-ENOENT);
}

void *iwl_uefi_get_reduced_power(struct iwl_trans *trans, size_t *len)
{
	struct efivar_entry *reduce_power_efivar;
	struct pnvm_sku_package *package;
	void *data = NULL;
	unsigned long package_size;
	int err;

	*len = 0;

	reduce_power_efivar = kzalloc(sizeof(*reduce_power_efivar), GFP_KERNEL);
	if (!reduce_power_efivar)
		return ERR_PTR(-ENOMEM);

	memcpy(&reduce_power_efivar->var.VariableName, IWL_UEFI_REDUCED_POWER_NAME,
	       sizeof(IWL_UEFI_REDUCED_POWER_NAME));
	reduce_power_efivar->var.VendorGuid = IWL_EFI_VAR_GUID;

	/*
	 * TODO: we hardcode a maximum length here, because reading
	 * from the UEFI is not working.  To implement this properly,
	 * we have to call efivar_entry_size().
	 */
	package_size = IWL_HARDCODED_REDUCE_POWER_SIZE;

	package = kmalloc(package_size, GFP_KERNEL);
	if (!package) {
		package = ERR_PTR(-ENOMEM);
		goto out;
	}

	err = efivar_entry_get(reduce_power_efivar, NULL, &package_size, package);
	if (err) {
		IWL_DEBUG_FW(trans,
			     "Reduced Power UEFI variable not found %d (len %lu)\n",
			     err, package_size);
		kfree(package);
		data = ERR_PTR(err);
		goto out;
	}

	IWL_DEBUG_FW(trans, "Read reduced power from UEFI with size %lu\n",
		     package_size);
	*len = package_size;

	IWL_DEBUG_FW(trans, "rev %d, total_size %d, n_skus %d\n",
		     package->rev, package->total_size, package->n_skus);

	data = iwl_uefi_reduce_power_parse(trans, package->data,
					   *len - sizeof(*package));

	kfree(package);

out:
	kfree(reduce_power_efivar);

	return data;
}

#ifdef CONFIG_ACPI
static int iwl_uefi_sgom_parse(struct uefi_cnv_wlan_sgom_data *sgom_data,
			       struct iwl_fw_runtime *fwrt)
{
	int i, j;

	if (sgom_data->revision != 1)
		return -EINVAL;

	memcpy(fwrt->sgom_table.offset_map, sgom_data->offset_map,
	       sizeof(fwrt->sgom_table.offset_map));

	for (i = 0; i < MCC_TO_SAR_OFFSET_TABLE_ROW_SIZE; i++) {
		for (j = 0; j < MCC_TO_SAR_OFFSET_TABLE_COL_SIZE; j++) {
			/* since each byte is composed of to values, */
			/* one for each letter, */
			/* extract and check each of them separately */
			u8 value = fwrt->sgom_table.offset_map[i][j];
			u8 low = value & 0xF;
			u8 high = (value & 0xF0) >> 4;

			if (high > fwrt->geo_num_profiles)
				high = 0;
			if (low > fwrt->geo_num_profiles)
				low = 0;
			fwrt->sgom_table.offset_map[i][j] = (high << 4) | low;
		}
	}

	fwrt->sgom_enabled = true;
	return 0;
}

void iwl_uefi_get_sgom_table(struct iwl_trans *trans,
			     struct iwl_fw_runtime *fwrt)
{
	struct efivar_entry *sgom_efivar;
	struct uefi_cnv_wlan_sgom_data *data;
	unsigned long package_size;
	int err, ret;

	if (!fwrt->geo_enabled)
		return;

	sgom_efivar = kzalloc(sizeof(*sgom_efivar), GFP_KERNEL);
	if (!sgom_efivar)
		return;

	memcpy(&sgom_efivar->var.VariableName, IWL_UEFI_SGOM_NAME,
	       sizeof(IWL_UEFI_SGOM_NAME));
	sgom_efivar->var.VendorGuid = IWL_EFI_VAR_GUID;

	/* TODO: we hardcode a maximum length here, because reading
	 * from the UEFI is not working.  To implement this properly,
	 * we have to call efivar_entry_size().
	 */
	package_size = IWL_HARDCODED_SGOM_SIZE;

	data = kmalloc(package_size, GFP_KERNEL);
	if (!data) {
		data = ERR_PTR(-ENOMEM);
		goto out;
	}

	err = efivar_entry_get(sgom_efivar, NULL, &package_size, data);
	if (err) {
		IWL_DEBUG_FW(trans,
			     "SGOM UEFI variable not found %d\n", err);
		goto out_free;
	}

	IWL_DEBUG_FW(trans, "Read SGOM from UEFI with size %lu\n",
		     package_size);

	ret = iwl_uefi_sgom_parse(data, fwrt);
	if (ret < 0)
		IWL_DEBUG_FW(trans, "Cannot read SGOM tables. rev is invalid\n");

out_free:
	kfree(data);

out:
	kfree(sgom_efivar);
}
IWL_EXPORT_SYMBOL(iwl_uefi_get_sgom_table);
#endif /* CONFIG_ACPI */
