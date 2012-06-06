/*
 * Copyright (c) 2011 Intel Corporation
 *
 * by
 *    Maciej Patelczyk <mpatelcz@gkslx007.igk.intel.com>
 *    Krzysztof Wierzbicki <krzysztof.wierzbicki@intel.com>
 *    Patrick Porlan <patrick.porlan@intel.com>
 *    Nisheeth Bhat <nisheeth.bhat@intel.com>
 *    Keith Busch <keith.busch@intel.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>
 */

#include "nvme.h"
#include "nvme_debug.h"
#include <sys/mman.h>



void nvme_dma_mem_read(target_phys_addr_t addr, uint8_t *buf, int len)
{
    cpu_physical_memory_rw(addr, buf, len, 0);
}

void nvme_dma_mem_write(target_phys_addr_t addr, uint8_t *buf, int len)
{
    cpu_physical_memory_rw(addr, buf, len, 1);
}

static uint8_t do_rw_prp(NVMEState *n, uint64_t mem_addr, uint64_t *data_size_p,
    uint64_t *file_offset_p, uint8_t *mapping_addr, uint8_t rw)
{
    uint64_t data_len;

    if (*data_size_p == 0) {
        return FAIL;
    }

    /* Data Len to be written per page basis */
    data_len = PAGE_SIZE - (mem_addr % PAGE_SIZE);
    if (data_len > *data_size_p) {
        data_len = *data_size_p;
    }

    LOG_DBG("File offset for read/write:%ld", *file_offset_p);
    LOG_DBG("Length for read/write:%ld", data_len);
    LOG_DBG("Address for read/write:%ld", mem_addr);

    switch (rw) {
    case NVME_CMD_READ:
        LOG_DBG("Read cmd called");
        nvme_dma_mem_write(mem_addr, (mapping_addr + *file_offset_p), data_len);
        break;
    case NVME_CMD_WRITE:
        LOG_DBG("Write cmd called");
        nvme_dma_mem_read(mem_addr, (mapping_addr + *file_offset_p), data_len);
        break;
    default:
        LOG_ERR("Error- wrong opcode: %d", rw);
        return FAIL;
    }
    *file_offset_p = *file_offset_p + data_len;
    *data_size_p = *data_size_p - data_len;
    return NVME_SC_SUCCESS;
}

static uint8_t do_rw_prp_list(NVMEState *n, NVMECmd *command,
    uint64_t *data_size_p, uint64_t *file_offset_p, uint8_t *mapping_addr)
{
    uint64_t prp_list[512], prp_entries;
    uint16_t i;
    uint8_t res = NVME_SC_SUCCESS;
    NVME_rw *cmd = (NVME_rw *)command;

    LOG_DBG("Data Size remaining for read/write:%ld", *data_size_p);

    /* Logic to find the number of PRP Entries */
    prp_entries = (uint64_t) ((*data_size_p + PAGE_SIZE - 1) / PAGE_SIZE);
    nvme_dma_mem_read(cmd->prp2, (uint8_t *)prp_list,
        min(sizeof(prp_list), prp_entries * sizeof(uint64_t)));

    i = 0;
    /* Read/Write on PRPList */
    while (*data_size_p != 0) {
        if (i == 511 && *data_size_p > PAGE_SIZE) {
            /* Calculate the actual number of remaining entries */
            prp_entries = (uint64_t) ((*data_size_p + PAGE_SIZE - 1) /
                PAGE_SIZE);
            nvme_dma_mem_read(prp_list[511], (uint8_t *)prp_list,
                min(sizeof(prp_list), prp_entries * sizeof(uint64_t)));
            i = 0;
        }

        res = do_rw_prp(n, prp_list[i], data_size_p,
            file_offset_p, mapping_addr, cmd->opcode);
        LOG_DBG("Data Size remaining for read/write:%ld", *data_size_p);
        if (res == FAIL) {
            break;
        }
        i++;
    }
    return res;
}

/*********************************************************************
    Function     :    update_ns_util
    Description  :    Updates the Namespace Utilization
                      of NVME disk
    Return Type  :    void

    Arguments    :    NVMEState * : Pointer to NVME device State
                      struct NVME_rw * : NVME IO command
*********************************************************************/
static void update_ns_util(DiskInfo *disk, uint64_t slba, uint16_t nlb)
{
    uint64_t nr;
    unsigned long *addr = (unsigned long *)disk->ns_util;

    /* Update the namespace utilization */
    for (nr = slba; nr <= nlb + slba; ++nr) {
        if (test_and_set_bit(nr, addr) == 0) {
            ++disk->idtfy_ns.nuse;
        }
    }
}

static uint8_t check_read_blocks(DiskInfo *disk, uint64_t slba, uint64_t nlb)
{
    uint64_t nr;
    int partial = 0;
    int empty = 1;
    unsigned long *addr = (unsigned long *)disk->ns_util;

    if (!(disk->idtfy_ns.nsfeat & NVME_NSFEAT_READ_ERR)) {
        return 0;
    }

    for (nr = slba; nr <= nlb + slba; ++nr) {
        if (test_bit(nr, addr)) {
            empty = 0;
        } else {
            partial = 1;
        }
    }

    return empty ? NVME_AON_READ_UNALLOCATED_BLOCK :
        partial ? NVME_AON_READ_PARTIAL_UNALLOCATED_BLOCK : 0;
}

static void nvme_update_stats(NVMEState *n, DiskInfo *disk, uint8_t opcode,
    uint64_t slba, uint16_t nlb)
{
    uint64_t tmp;
    if (opcode == NVME_CMD_WRITE) {
        uint64_t old_use = disk->idtfy_ns.nuse;

        update_ns_util(disk, slba, nlb);

        /* check if there needs to be an event issued */
        if (old_use != disk->idtfy_ns.nuse && !disk->thresh_warn_issued &&
                (100 - (uint32_t)((((double)disk->idtfy_ns.nuse) /
                    disk->idtfy_ns.nsze) * 100) < NVME_SPARE_THRESH)) {
            LOG_NORM("Device:%d nsid:%d, setting threshold warning",
                n->instance, disk->nsid);
            disk->thresh_warn_issued = 1;
            enqueue_async_event(n, event_type_smart,
                event_info_smart_spare_thresh, NVME_LOG_SMART_INFORMATION);
        }

        if (++disk->host_write_commands[0] == 0) {
            ++disk->host_write_commands[1];
        }
        disk->write_data_counter += nlb + 1;
        tmp = disk->data_units_written[0];
        disk->data_units_written[0] += (disk->write_data_counter / 1000);
        disk->write_data_counter %= 1000;
        if (tmp > disk->data_units_written[0]) {
            ++disk->data_units_written[1];
        }
    } else if (opcode == NVME_CMD_READ) {
        if (++disk->host_read_commands[0] == 0) {
            ++disk->host_read_commands[1];
        }
        disk->read_data_counter += nlb + 1;
        tmp = disk->data_units_read[0];
        disk->data_units_read[0] += (disk->read_data_counter / 1000);
        disk->read_data_counter %= 1000;
        if (tmp > disk->data_units_read[0]) {
            ++disk->data_units_read[1];
        }
    }
}

uint8_t nvme_io_command(NVMEState *n, NVMECmd *sqe, NVMECQE *cqe,
    NVMEIOSQueue *sq)
{
    NVME_rw *e = (NVME_rw *)sqe;
    NVMEStatusField *sf = (NVMEStatusField *)&cqe->status;
    uint8_t res;
    uint64_t data_size, file_offset;
    uint8_t *mapping_addr;
    uint32_t nvme_blk_sz;
    DiskInfo *disk;
    uint8_t lba_idx;

    sf->sc = NVME_SC_SUCCESS;
    LOG_DBG("%s(): called", __func__);

    if (!security_state_unlocked(n)) {
        LOG_NORM("%s(): invalid security state '%c' for IO", __func__, n->s);
        sf->sc = NVME_SC_CMD_SEQ_ERROR;
        return FAIL;
    }

    /* As of NVMe spec rev 1.0b "All NVM cmds use the CMD.DW1 (NSID) field".
     * Thus all NVM cmd set cmds must check for illegal namespaces up front */
    if (e->nsid == 0 || (e->nsid > n->idtfy_ctrl->nn)) {
        LOG_NORM("%s(): Invalid nsid:%u", __func__, e->nsid);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }

    if (sqe->opcode == NVME_CMD_FLUSH) {
        return NVME_SC_SUCCESS;
    } else if (sqe->opcode == NVME_CMD_DSM) {
        return NVME_SC_SUCCESS;
    } else if ((sqe->opcode != NVME_CMD_READ) &&
             (sqe->opcode != NVME_CMD_WRITE)) {
        LOG_NORM("%s(): Wrong IO opcode:%#02x", __func__, sqe->opcode);
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }

    disk = n->disk[e->nsid - 1];
    if ((e->slba + e->nlb) >= disk->idtfy_ns.nsze) {
        LOG_NORM("%s(): LBA out of range", __func__);
        sf->sc = NVME_SC_LBA_RANGE;
        return FAIL;
    } else if ((e->slba + e->nlb) >= disk->idtfy_ns.ncap) {
        LOG_NORM("%s():Capacity Exceeded", __func__);
        sf->sc = NVME_SC_CAP_EXCEEDED;
        return FAIL;
    }
    lba_idx = disk->idtfy_ns.flbas & 0xf;
    if ((e->mptr == 0) &&            /* if NOT supplying separate meta buffer */
        (disk->idtfy_ns.lbaf[lba_idx].ms != 0) &&       /* if using metadata */
        ((disk->idtfy_ns.flbas & 0x10) == 0)) {   /* if using separate buffer */

        LOG_ERR("%s(): no meta-data given for separate buffer", __func__);
        sf->sc = NVME_SC_INVALID_FIELD;
        return FAIL;
    }

    /* Read in the command */
    nvme_blk_sz = NVME_BLOCK_SIZE(disk->idtfy_ns.lbaf[lba_idx].lbads);
    LOG_DBG("NVME Block size: %u", nvme_blk_sz);
    data_size = (e->nlb + 1) * nvme_blk_sz;

    file_offset = e->slba * nvme_blk_sz;
    mapping_addr = disk->mapping_addr;

    if (n->fultondale) {
        uint32_t boundary = fultondale_boundary[n->fultondale];
        if ((file_offset % boundary) + data_size > boundary) {
            LOG_ERR("%s(): crossed io boundary:%d slba:%ld nlb:%d",
                __func__, boundary, e->slba, e->nlb);
        }
    }
    if (disk->idtfy_ns.flbas & 0x10) {
        data_size += (disk->idtfy_ns.lbaf[lba_idx].ms * (e->nlb + 1));
    }

    if (n->idtfy_ctrl->mdts && data_size > PAGE_SIZE *
                (1 << (n->idtfy_ctrl->mdts))) {
        LOG_ERR("%s(): data size:%ld exceeds max:%ld", __func__,
            data_size, ((uint64_t)PAGE_SIZE) * (1 << (n->idtfy_ctrl->mdts)));
        sf->sc = NVME_SC_INVALID_FIELD;
        sf->dnr = 1;
        return FAIL;
    }

    /* Namespace not ready */
    if (mapping_addr == NULL) {
        LOG_NORM("%s():Namespace not ready", __func__);
        sf->sc = NVME_SC_NS_NOT_READY;
        return FAIL;
    }
    if (n->drop_rate && random_chance(n->drop_rate)) {
        LOG_NORM("dropping random command:%d", sqe->cid);
        CommandEntry *ce = qemu_malloc(sizeof(CommandEntry));
        ce->cid = sqe->cid;
        QTAILQ_INSERT_TAIL(&(sq->cmd_list), ce, entry);
        return NVME_NO_COMPLETE;
    }
    if (n->fail_rate && random_chance(n->fail_rate)) {
        LOG_NORM("randomly failing command:%d", sqe->cid);
        sf->sc = NVME_SC_NS_NOT_READY;
        return FAIL;
    }
    if (!QTAILQ_EMPTY(&n->media_err_list)) {
        NVMEIoError *me;
        uint64_t elba = e->slba + e->nlb;
        QTAILQ_FOREACH(me, &n->media_err_list, entry) {
            if ((me->slba >= e->slba && me->slba <= elba) ||
                (me->elba >= e->slba && me->elba <= elba)) {
                switch (me->io_error) {
                case NVME_MEDIA_ERR_WRITE:
                    if (sqe->opcode == NVME_CMD_WRITE) {
                        sf->sc = NVME_WRITE_FAULT;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_READ:
                    if (sqe->opcode == NVME_CMD_READ) {
                        sf->sc = NVME_UNRECOVERED_READ_ER;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_GUARD:
                    if (((NVMECmdRead *)sqe)->prinfo & 0x4) {
                        sf->sc = NVME_END_TO_END_GUARD_CHECK_ER;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_APP_TAG:
                    if (((NVMECmdRead *)sqe)->prinfo & 0x2) {
                        sf->sc = NVME_END_TO_END_APPLICATION_TAG_CHECK_ER;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_REF_TAG:
                    if (((NVMECmdRead *)sqe)->prinfo & 0x1) {
                        sf->sc = NVME_END_TO_END_REFERENCE_TAG_CHECK_ER;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_COMPARE:
                    if (sqe->opcode != NVME_CMD_COMPARE) {
                        sf->sc = NVME_COMPARE_FAILURE;
                        return 0;
                    }
                    break;
                case NVME_MEDIA_ERR_ACCESS:
                    sf->sc = NVME_ACCESS_DENIED;
                    return 0;
                default:
                    break;
                }
            }
        }
    }
    if (n->timeout_error != NULL) {
        NVMEIoError *me = n->timeout_error;
        uint64_t elba = e->slba + e->nlb;
        if (((me->slba >= e->slba && me->slba <= elba) ||
             (me->elba >= e->slba && me->elba <= elba)) &&
            ((me->io_error == NVME_TO_WRITE && sqe->opcode == NVME_CMD_WRITE) ||
             (me->io_error == NVME_TO_READ  && sqe->opcode == NVME_CMD_READ) ||
             (me->io_error == NVME_TO_IO))) {
            LOG_NORM("dropping qid:%d command:%d", sq->id, sqe->cid);
            CommandEntry *ce = qemu_malloc(sizeof(CommandEntry));
            ce->cid = sqe->cid;
            QTAILQ_INSERT_TAIL(&(sq->cmd_list), ce, entry);
            qemu_free(n->timeout_error);
            n->timeout_error = NULL;
            return NVME_NO_COMPLETE;
        }
    }

    if (e->opcode == NVME_CMD_READ) {
        uint8_t sc = check_read_blocks(disk, e->slba, e->nlb);
        if (sc != 0) {
            sf->sct = NVME_SCT_CMD_SPEC_ERR;
            sf->sc = sc;
            sf->dnr = 1;
            return FAIL;
        }
    }
    /* Writing/Reading PRP1 */
    res = do_rw_prp(n, e->prp1, &data_size, &file_offset, mapping_addr,
        e->opcode);
    if (res == FAIL) {
        return FAIL;
    }
    if (data_size > 0) {
        if (data_size <= PAGE_SIZE) {
            res = do_rw_prp(n, e->prp2, &data_size, &file_offset, mapping_addr,
                e->opcode);
        } else {
            res = do_rw_prp_list(n, sqe, &data_size, &file_offset,
                mapping_addr);
        }
        if (res == FAIL) {
            return FAIL;
        }
    }

    /* Spec states that non-zero meta data buffers shall be ignored, i.e. no
     * error reported, when the DW4&5 (MPTR) field is not in use */
    if ((e->mptr != 0) &&                /* if supplying separate meta buffer */
        (disk->idtfy_ns.lbaf[lba_idx].ms != 0) &&       /* if using metadata */
        ((disk->idtfy_ns.flbas & 0x10) == 0)) {   /* if using separate buffer */

        /* Then go ahead and use the separate meta data buffer */
        unsigned int ms, meta_offset, meta_size;
        uint8_t *meta_mapping_addr;

        ms = disk->idtfy_ns.lbaf[disk->idtfy_ns.flbas].ms;
        meta_offset = e->slba * ms;
        meta_size = (e->nlb + 1) * ms;
        meta_mapping_addr = disk->meta_mapping_addr + meta_offset;

        if (e->opcode == NVME_CMD_READ) {
            nvme_dma_mem_write(e->mptr, meta_mapping_addr, meta_size);
        } else if (e->opcode == NVME_CMD_WRITE) {
            nvme_dma_mem_read(e->mptr, meta_mapping_addr, meta_size);
        }
    }

    nvme_update_stats(n, disk, e->opcode, e->slba, e->nlb);
    return res;
}

uint8_t nvme_aon_io_command(NVMEState *n, NVMECmd *sqe, NVMECQE *cqe,
    uint32_t pdid)
{
    NVMEAonUserRwCmd *rw    = (NVMEAonUserRwCmd *)sqe;
    NVMEStatusField  *sf    = (NVMEStatusField *)&cqe->status;
    NVMEAonNStag     *nstag = NULL;
    NVMEAonStag      *stag  = NULL;
    NVMEAonStag      *mstag = NULL;
    NVMEAonPD        *pd    = NULL;
    DiskInfo         *disk  = NULL;

    uint8_t *mapping_addr;
    uint64_t data_size, file_offset, prp_offset, prp_index, prp_entries;
    uint16_t blksize;

    if (!security_state_unlocked(n)) {
        LOG_NORM("%s(): invalid security state for IO", __func__);
        sf->sc = NVME_SC_CMD_SEQ_ERROR;
        return FAIL;
    }
    if (sqe->opcode == AON_CMD_USER_FLUSH) {
        return NVME_SC_SUCCESS;
    }
    if (sqe->opcode != AON_CMD_USER_WRITE &&
        sqe->opcode != AON_CMD_USER_READ) {
        sf->sc = NVME_SC_INVALID_OPCODE;
        return FAIL;
    }
    if (rw->nstag == 0 || rw->nstag > n->aon_ctrl_vs->mnon) {
        LOG_NORM("%s(): invalid nstag %d", __func__, rw->nstag);
        sf->sc = NVME_AON_INVALID_NAMESPACE_TAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (n->nstags[rw->nstag - 1] == NULL) {
        LOG_NORM("%s(): unallocated nstag %d", __func__, rw->nstag);
        sf->sc = NVME_AON_INVALID_NAMESPACE_TAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (pdid == 0 || pdid > n->aon_ctrl_vs->mnpd) {
        LOG_NORM("%s(): Invalid pdid %d", __func__, pdid);
        sf->sc = NVME_AON_INVALID_PROTECTION_DOMAIN_IDENTIFIER;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (n->protection_domains[pdid - 1] == NULL) {
        LOG_ERR("%s(): pdid %d not allocated", __func__, pdid);
        sf->sc = NVME_AON_INVALID_PROTECTION_DOMAIN_IDENTIFIER;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (rw->stag == 0 || rw->stag > n->aon_ctrl_vs->mnhr) {
        LOG_NORM("%s(): Invalid stag %d", __func__, rw->stag);
        sf->sc = NVME_AON_INVALID_STAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (n->stags[rw->stag - 1] == NULL) {
        LOG_NORM("%s(): stag %d not allocated", __func__, rw->stag);
        sf->sc = NVME_AON_INVALID_STAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (rw->mdstag && rw->mdstag > n->aon_ctrl_vs->mnhr) {
        LOG_NORM("%s(): Invalid meta-stag %d", __func__, rw->mdstag);
        sf->sc = NVME_AON_INVALID_STAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }
    if (rw->mdstag && n->stags[rw->mdstag - 1] == NULL) {
        LOG_NORM("%s(): meta-stag %d not allocated", __func__, rw->stag);
        sf->sc = NVME_AON_INVALID_STAG;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }

    nstag = n->nstags[rw->nstag - 1];
    stag = n->stags[rw->stag - 1];
    pd = n->protection_domains[pdid - 1];
    if (rw->mdstag) {
        mstag = n->stags[rw->mdstag - 1];
    }

    if (nstag->nsid == 0 || nstag->nsid > n->num_namespaces) {
        LOG_ERR("%s(): bad nsid referenced nstag", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }
    if (n->disk[nstag->nsid - 1] == NULL) {
        LOG_ERR("%s(): nsid unallocated", __func__);
        sf->sc = NVME_SC_INVALID_NAMESPACE;
        return FAIL;
    }
    if (stag->pdid != pdid) {
        LOG_NORM("%s(): queue pdid:%d not match stag pdid:%d", __func__, pdid,
            stag->pdid);
        sf->sc = NVME_AON_CONFLICTING_ATTRIBUTES;
        sf->sct = NVME_SCT_CMD_SPEC_ERR;
        return FAIL;
    }

    disk = n->disk[nstag->nsid - 1];
    if ((rw->slba + rw->nlb) >= disk->idtfy_ns.nsze) {
        LOG_NORM("%s(): LBA out of range", __func__);
        sf->sc = NVME_SC_LBA_RANGE;
        return FAIL;
    } else if ((rw->slba + rw->nlb) >= disk->idtfy_ns.ncap) {
        LOG_NORM("%s(): Capacity Exceeded", __func__);
        sf->sc = NVME_SC_CAP_EXCEEDED;
        return FAIL;
    }

    blksize = NVME_BLOCK_SIZE(disk->idtfy_ns.lbaf[
        disk->idtfy_ns.flbas].lbads);
    data_size = (rw->nlb + 1) * blksize;
    file_offset = rw->slba * blksize;

    if (disk->mapping_addr == NULL) {
        LOG_NORM("%s(): Namespace not ready", __func__);
        sf->sc = NVME_SC_NS_NOT_READY;
        return FAIL;
    }

    mapping_addr = disk->mapping_addr + file_offset;

    prp_entries = (uint64_t) ((data_size + stag->smps - 1) / stag->smps);
    prp_offset = rw->sto * blksize;
    prp_index = prp_offset / stag->smps;

    if (prp_index + prp_entries > stag->nmp) {
        LOG_NORM("%s(): stag offset:%lu for %lu prps beyond prp list:%lu",
            __func__, rw->sto, prp_entries, stag->nmp);
        sf->sc = NVME_AON_CONFLICTING_ATTRIBUTES;
        return FAIL;
    }
    {
        int i = 0;
        uint64_t prp_list[prp_entries];
        uint64_t data_len, mem_addr;

        nvme_dma_mem_read(stag->prp + prp_index * stag->smps,
            (uint8_t *)prp_list, prp_entries * sizeof(uint64_t));

        mem_addr = prp_list[0] + (prp_offset % stag->smps);
        data_len = stag->smps - (mem_addr % stag->smps);

        if (data_len > data_size) {
            data_len = data_size;
        }

        if (sqe->opcode == AON_CMD_USER_WRITE) {
            nvme_dma_mem_read(mem_addr, mapping_addr, data_len);
        } else if (sqe->opcode == AON_CMD_USER_READ) {
            nvme_dma_mem_write(mem_addr, mapping_addr, data_len);
        }
        mapping_addr += data_len;

        data_size -= data_len;
        for (i = 1; i < prp_entries && data_size > 0; i++) {
            mem_addr = prp_list[i];
            data_len = stag->smps;
            if (data_len > data_size) {
                data_len = data_size;
            }

            if (sqe->opcode == AON_CMD_USER_WRITE) {
                nvme_dma_mem_read(mem_addr, mapping_addr, data_len);
            } else if (sqe->opcode == AON_CMD_USER_READ) {
                nvme_dma_mem_write(mem_addr, mapping_addr, data_len);
            }
            data_size -= data_len;
            mapping_addr += data_len;
        }

        if (data_size != 0 || i != prp_entries) {
            LOG_ERR("%s(): bad transfer, prp entries:%lu, index:%d, and data"\
                    "size:%lu", __func__, prp_entries, i, data_size);
        }
    }
    nvme_update_stats(n, disk, rw->opcode & 0x3, rw->slba, rw->nlb);

    if (mstag != NULL) {
        uint64_t  ms, meta_offset, meta_size, mprp_offset, mprp_index,
            mprp_entries;
        uint8_t *meta_mapping_addr;

        if (disk->meta_mapping_addr == NULL) {
            LOG_NORM("%s(): meta data requested for disk with no meta-data",
                __func__);
            return FAIL;
        }

        ms = disk->idtfy_ns.lbaf[disk->idtfy_ns.flbas].ms;
        meta_offset = rw->slba * ms;
        meta_size = (rw->nlb + 1) * ms;
        meta_mapping_addr = disk->meta_mapping_addr + meta_offset;

        mprp_entries = (uint64_t) ((meta_size + mstag->smps - 1) / mstag->smps);
        mprp_offset = rw->mdsto * ms;
        mprp_index = mprp_offset / mstag->smps;

        if (mprp_index + mprp_entries > mstag->nmp) {
            LOG_NORM("%s(): stag offset:%lu for %lu prps beyond prp list:%lu",
                __func__, rw->sto, prp_entries, stag->nmp);
            sf->sc = NVME_AON_CONFLICTING_ATTRIBUTES;
            return FAIL;
        }
        {
            int i = 0;
            uint64_t prp_list[mprp_entries];
            uint64_t data_len, mem_addr;

            nvme_dma_mem_read(mstag->prp + mprp_index * mstag->smps,
                (uint8_t *)prp_list, mprp_entries * sizeof(uint64_t));

            mem_addr = prp_list[0] + (mprp_offset % mstag->smps);
            data_len = mstag->smps - (mem_addr % mstag->smps);
            if (data_len > meta_size) {
                data_len = meta_size;
            }
            if (rw->opcode == AON_CMD_USER_WRITE) {
                nvme_dma_mem_write(mem_addr, meta_mapping_addr, meta_size);
            } else if (rw->opcode == AON_CMD_USER_READ) {
                nvme_dma_mem_read(mem_addr, meta_mapping_addr, meta_size);
            }
            meta_size -= data_len;

            for (i = 1; i < prp_entries && meta_size > 0; i++) {
                mem_addr = prp_list[i];
                data_len = mstag->smps;
                if (data_len > meta_size) {
                    data_len = meta_size;
                }

                if (sqe->opcode == AON_CMD_USER_WRITE) {
                    nvme_dma_mem_read(mem_addr, meta_mapping_addr, data_len);
                } else if (sqe->opcode == AON_CMD_USER_READ) {
                    nvme_dma_mem_write(mem_addr, meta_mapping_addr, data_len);
                }
                meta_size -= data_len;
            }

            if (meta_size != 0 || i != prp_entries) {
                LOG_ERR("%s(): bad transfer, prp entries:%lu, index:%d, and "\
                        "data size:%lu",
                    __func__, prp_entries, i, meta_size);
            }
        }
    }

    return NVME_SC_SUCCESS;
}

/* brnl stuff */
#include <dirent.h>

void *xrealloc(void);
int make_fs(void);
uint32_t write_sb(uint32_t root, uint32_t free);
struct brnl_dirent *
write_inode(char *name, uint32_t dirno, uint32_t prev, uint32_t region);
uint32_t write_file(char *name, uint32_t dirno, uint32_t prev, uint32_t region);
uint32_t write_dir(char *name, uint32_t dirno, uint32_t prev, uint32_t
    fake_region);

enum {
    BRNL_DIR_REGION = 1,
    BRNL_SUPER_MAGIC = 1818324545,
};

struct brnl_superblock {
    uint32_t root_dir_fake_inum;
    uint32_t root_dir_offset;
    uint32_t free_space_offset;
    uint32_t fake_inums_offset;
};

struct brnl_dirent {
    char name[256];
    uint32_t ino;   /* The fake inode this block belongs to */
    uint8_t len;
    uint8_t type;
    uint8_t pad[2];
    uint32_t prev;
    uint32_t next;
    uint32_t region_id; /* Faked for non-files */
    union {
        struct {
            uint32_t brnl_dir_offset;
        } dir;
    };
};

void *data = NULL;
uint32_t current = 0;

void *xrealloc(void)
{
    data = qemu_realloc(data, 512 * (current + 1));
    if (!data) {
        exit(1);
    }
    memset(data + 512 * current, 0, 512);
    return data + 512 * current;
}

uint32_t write_sb(uint32_t root, uint32_t free)
{
    struct brnl_superblock *sb = xrealloc();
    sb->root_dir_offset = root;
    sb->free_space_offset = free;
    sb->fake_inums_offset = 2;
    current = 3;
    return current;
}

struct brnl_dirent *
write_inode(char *name, uint32_t dirno, uint32_t prev, uint32_t region)
{
    struct brnl_dirent *de = xrealloc();
    struct brnl_dirent *de_prev = data + 512 * prev;
    de->prev = prev;
    if (prev) {
        de_prev->next = current;
    }
    strcpy(de->name, name);
    de->ino = dirno;
    de->len = strlen(name);
    de->region_id = region;
    return de;
}

uint32_t write_file(char *name, uint32_t dirno, uint32_t prev, uint32_t region)
{
    struct brnl_dirent *de = write_inode(name, dirno, prev, region);
    de->type = DT_REG;
    return current++;
}

uint32_t write_dir(char *name, uint32_t dirno, uint32_t prev, uint32_t fake_region)
{
    struct brnl_dirent *de = write_inode(name, dirno, prev, fake_region);
    de->type = DT_DIR;
    de->dir.brnl_dir_offset = current + 1;
    return current++;
}

int make_fs(void)
{
    uint32_t prev;
    prev = write_sb(3, 1);
    prev = write_dir((char *)"..", BRNL_DIR_REGION, 0, BRNL_DIR_REGION);
    prev = write_file((char *)"apple", BRNL_DIR_REGION, prev, 4);
    return current * 512;
}

/* end brnl stuff */

static int nvme_create_meta_disk(uint32_t instance, uint32_t nsid,
    DiskInfo *disk)
{
    uint32_t ms;
    ms = disk->idtfy_ns.lbaf[disk->idtfy_ns.flbas].ms;
    if (ms != 0 && !(disk->idtfy_ns.flbas & 0x10)) {
        char str[64];
        uint64_t blks, msize;

        snprintf(str, sizeof(str), "nvme_meta%d_n%d.img", instance, nsid);
        blks = disk->idtfy_ns.ncap;
        msize = blks * ms;

        disk->mfd = open(str, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (disk->mfd < 0) {
            LOG_ERR("Error while creating the meta-storage");
            return FAIL;
        }
        if (posix_fallocate(disk->mfd, 0, msize) != 0) {
            LOG_ERR("Error while allocating meta-data space for namespace");
            return FAIL;
        }
        disk->meta_mapping_addr = mmap(NULL, msize, PROT_READ | PROT_WRITE,
            MAP_SHARED, disk->mfd, 0);
        if (disk->meta_mapping_addr == NULL) {
            LOG_ERR("Error while opening namespace meta-data: %d", disk->nsid);
            return FAIL;
        }
        disk->meta_mapping_size = msize;
        memset(disk->meta_mapping_addr, 0xff, msize);
    } else {
        disk->meta_mapping_addr = NULL;
        disk->meta_mapping_size = 0;
    }
    return SUCCESS;
}

/*********************************************************************
    Function     :    nvme_create_storage_disk
    Description  :    Creates a NVME Storage Disk and the
                      namespaces within
    Return Type  :    int (0:1 Success:Failure)

    Arguments    :    uint32_t : instance number of the nvme device
                      uint32_t : namespace id
                      DiskInfo * : NVME disk to create storage for
*********************************************************************/
int nvme_create_storage_disk(uint32_t instance, uint32_t nsid, DiskInfo *disk,
    NVMEState *n)
{
    uint32_t blksize, lba_idx;
    uint64_t size, blks;
    char str[64];

    snprintf(str, sizeof(str), "nvme_disk%d_n%d.img", instance, nsid);
    disk->nsid = nsid;

    disk->fd = open(str, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (disk->fd < 0) {
        LOG_ERR("Error while creating the storage");
        return FAIL;
    }

    lba_idx = disk->idtfy_ns.flbas & 0xf;
    blks = disk->idtfy_ns.ncap;
    blksize = NVME_BLOCK_SIZE(disk->idtfy_ns.lbaf[lba_idx].lbads);
    size = blks * blksize;
    if (disk->idtfy_ns.flbas & 0x10) {
        /* extended lba */
        size += (blks * disk->idtfy_ns.lbaf[lba_idx].ms);
    }
    if (size == 0) {
        return SUCCESS;
    }
    if (posix_fallocate(disk->fd, 0, size) != 0) {
        LOG_ERR("Error while allocating space for namespace");
        return FAIL;
    }

    disk->mapping_addr = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_SHARED, disk->fd, 0);
    if (disk->mapping_addr == NULL) {
        LOG_ERR("Error while opening namespace: %d", disk->nsid);
        return FAIL;
    }
    disk->mapping_size = size;
    if (nvme_create_meta_disk(instance, nsid, disk) != SUCCESS) {
        return FAIL;
    }

    disk->ns_util = qemu_mallocz((disk->idtfy_ns.nsze + 7) / 8);
    if (disk->ns_util == NULL) {
        LOG_ERR("Error while reallocating the ns_util");
        return FAIL;
    }
    disk->thresh_warn_issued = 0;

    memset(&disk->range_type, 0x0, sizeof(disk->range_type));
    disk->range_type.nlb = disk->idtfy_ns.nsze;
    disk->range_type.guid[15] = 0xde;
    disk->range_type.guid[14] = 0xad;
    disk->range_type.guid[13] = 0xba;
    disk->range_type.guid[12] = 0xbe;
    disk->range_type.guid[1]  = instance;
    disk->range_type.guid[0]  = nsid;

    LOG_NORM("created disk storage, mapping_addr:%p size:%lu",
        disk->mapping_addr, disk->mapping_size);

    if (n->brnl && nsid == 1) {
        int len = make_fs();
        memcpy(disk->mapping_addr, data, len);
    }

    return SUCCESS;
}

/*********************************************************************
    Function     :    nvme_create_storage_disks
    Description  :    Creates a NVME Storage Disks and the
                      namespaces within
    Return Type  :    int (0:1 Success:Failure)

    Arguments    :    NVMEState * : Pointer to NVME device State
*********************************************************************/
int nvme_create_storage_disks(NVMEState *n)
{
    uint32_t i;
    int ret = SUCCESS;

    for (i = 0; i < n->num_namespaces; i++) {
        if (n->disk[i] == NULL) {
            continue;
        }
        ret = nvme_create_storage_disk(n->instance, i + 1, n->disk[i], n);
    }

    LOG_NORM("%s():Backing store created for instance %d", __func__,
        n->instance);

    return ret;
}

static int nvme_close_meta_disk(DiskInfo *disk)
{
    if (disk->meta_mapping_addr != NULL) {
        if (munmap(disk->meta_mapping_addr, disk->meta_mapping_size) < 0) {
            LOG_ERR("Error while closing meta namespace: %d", disk->nsid);
            return FAIL;
        } else {
            disk->meta_mapping_addr = NULL;
            disk->meta_mapping_size = 0;
            if (close(disk->mfd) < 0) {
                LOG_ERR("Unable to close the nvme disk");
                return FAIL;
            }
        }
    }
    return SUCCESS;
}

/*********************************************************************
    Function     :    nvme_close_storage_disk
    Description  :    Deletes NVME Storage Disk
    Return Type  :    int (0:1 Success:Failure)

    Arguments    :    DiskInfo * : Pointer to NVME disk
*********************************************************************/
int nvme_close_storage_disk(DiskInfo *disk)
{
    if (disk->mapping_addr != NULL) {
        if (munmap(disk->mapping_addr, disk->mapping_size) < 0) {
            LOG_ERR("Error while closing namespace: %d", disk->nsid);
            return FAIL;
        } else {
            disk->mapping_addr = NULL;
            disk->mapping_size = 0;
            if (close(disk->fd) < 0) {
                LOG_ERR("Unable to close the nvme disk");
                return FAIL;
            }
        }
    }
    if (disk->ns_util) {
        qemu_free(disk->ns_util);
        disk->ns_util = NULL;
    }
    if (nvme_close_meta_disk(disk) != SUCCESS) {
        return FAIL;
    }
    return SUCCESS;
}

/*********************************************************************
    Function     :    nvme_close_storage_disks
    Description  :    Closes the NVME Storage Disks and the
                      associated namespaces
    Return Type  :    int (0:1 Success:Failure)

    Arguments    :    NVMEState * : Pointer to NVME device State
*********************************************************************/
int nvme_close_storage_disks(NVMEState *n)
{
    uint32_t i;
    int ret = SUCCESS;

    for (i = 0; i < n->num_namespaces; i++) {
        if (n->disk[i] == NULL) {
            continue;
        }
        ret = nvme_close_storage_disk(n->disk[i]);
    }
    return ret;
}

