#include "replication/delete_queue.hpp"

#include "buffer_cache/buf_lock.hpp"
#include "buffer_cache/co_functions.hpp"
#include "logger.hpp"
#include "scoped_malloc.hpp"
#include "store.hpp"

namespace replication {

namespace delete_queue {

// The offset of the primal offset.
const int PRIMAL_OFFSET_OFFSET = sizeof(block_magic_t);
const int TIMESTAMPS_AND_OFFSETS_OFFSET = PRIMAL_OFFSET_OFFSET + sizeof(off64_t);
const int TIMESTAMPS_AND_OFFSETS_SIZE = sizeof(large_buf_ref) + 3 * sizeof(block_id_t);

off64_t *primal_offset(void *root_buffer) {
    return reinterpret_cast<off64_t *>(reinterpret_cast<char *>(root_buffer) + PRIMAL_OFFSET_OFFSET);
}

large_buf_ref *timestamps_and_offsets_largebuf(void *root_buffer) {
    char *p = reinterpret_cast<char *>(root_buffer);
    return reinterpret_cast<large_buf_ref *>(p + TIMESTAMPS_AND_OFFSETS_OFFSET);
}

large_buf_ref *keys_largebuf(void *root_buffer) {
    char *p = reinterpret_cast<char *>(root_buffer);
    return reinterpret_cast<large_buf_ref *>(p + (TIMESTAMPS_AND_OFFSETS_OFFSET + TIMESTAMPS_AND_OFFSETS_SIZE));
}

int keys_largebuf_ref_size(block_size_t block_size) {
    return block_size.value() - (TIMESTAMPS_AND_OFFSETS_OFFSET + TIMESTAMPS_AND_OFFSETS_SIZE);
}

// TODO: Add test doublechecking that sizeof(t_and_o) == 12.
struct t_and_o {
    repli_timestamp timestamp;
    off64_t offset;
} __attribute__((__packed__));

}  // namespace delete_queue

void add_key_to_delete_queue(boost::shared_ptr<transactor_t>& txor, block_id_t queue_root_id, repli_timestamp timestamp, const store_key_t *key) {
    thread_saver_t saver;

    // Beware: Right now, some aspects of correctness depend on the
    // fact that we hold the queue_root lock for the entire operation.
    buf_lock_t queue_root(saver, *txor, queue_root_id, rwi_write);

    // TODO this could be a non-major write?
    void *queue_root_buf = queue_root->get_data_major_write();

    off64_t primal_offset = *delete_queue::primal_offset(queue_root_buf);
    large_buf_ref *t_o_ref = delete_queue::timestamps_and_offsets_largebuf(queue_root_buf);
    large_buf_ref *keys_ref = delete_queue::keys_largebuf(queue_root_buf);

    rassert(t_o_ref->size % sizeof(delete_queue::t_and_o) == 0);

    // 1. Possibly update the (timestamp, offset) queue.  (This happens at most once per second.)
    {
        // TODO: Why must we allocate large_buf_t's with new?
        boost::scoped_ptr<large_buf_t> t_o_largebuf(new large_buf_t(txor, t_o_ref, lbref_limit_t(delete_queue::TIMESTAMPS_AND_OFFSETS_SIZE), rwi_write));

        if (t_o_ref->size == 0) {
            // The size is only zero in the unallocated state.  (Large
            // bufs can't actually handle size zero, so we can't let
            // the large buf shrink to that size.)
            delete_queue::t_and_o tao;
            tao.timestamp = timestamp;
            tao.offset = primal_offset + keys_ref->size;
            t_o_largebuf->allocate(sizeof(tao));
            t_o_largebuf->fill_at(0, &tao, sizeof(tao));
        } else {

            // TODO: Allow upgrade of large buf intent.
            co_acquire_large_buf(saver, t_o_largebuf.get());

            delete_queue::t_and_o last_tao;
            t_o_largebuf->read_at(t_o_ref->size - sizeof(last_tao), &last_tao, sizeof(last_tao));

            if (last_tao.timestamp.time > timestamp.time) {
                logWRN("The delete queue is receiving updates out of order (t1 = %d, t2 = %d), or the system clock has been set back!  Bringing up a replica may be excessively inefficient.\n", last_tao.timestamp.time, timestamp.time);

                // Timestamps must be monotonically increasing, so sorry.
                timestamp = last_tao.timestamp;
            }

            if (last_tao.timestamp.time != timestamp.time) {
                delete_queue::t_and_o tao;
                tao.timestamp = timestamp;
                tao.offset = primal_offset + keys_ref->size;
                int refsize_adjustment_dontcare;
                t_o_largebuf->append(sizeof(tao), &refsize_adjustment_dontcare);
                t_o_largebuf->fill_at(t_o_ref->size - sizeof(tao), &tao, sizeof(tao));
            }
        }

        // TODO: Remove old items from the front of t_o_largebuf.
    }

    // 2. Update the keys list.

    {
        boost::scoped_ptr<large_buf_t> keys_largebuf(new large_buf_t(txor, keys_ref, lbref_limit_t(delete_queue::keys_largebuf_ref_size((*txor)->cache->get_block_size())), rwi_write));

        if (keys_ref->size == 0) {
            keys_largebuf->allocate(1 + key->size);
            keys_largebuf->fill_at(0, key, 1 + key->size);
        } else {
            // TODO: acquire rhs, or lhs+rhs, something appropriate.
            co_acquire_large_buf(saver, keys_largebuf.get());

            int refsize_adjustment_dontcare;
            keys_largebuf->append(1 + key->size, &refsize_adjustment_dontcare);
            keys_largebuf->fill_at(keys_ref->size - (1 + key->size), key, 1 + key->size);
        }
    }
}

void dump_keys_from_delete_queue(boost::shared_ptr<transactor_t>& txor, block_id_t queue_root_id, repli_timestamp begin_timestamp, repli_timestamp end_timestamp, deletion_key_stream_receiver_t *recipient) {
    thread_saver_t saver;

    // Beware: Right now, some aspects of correctness depend on the
    // fact that we hold the queue_root lock for the entire operation.
    buf_lock_t queue_root(saver, *txor, queue_root_id, rwi_read);

    void *queue_root_buf = const_cast<void *>(queue_root->get_data_read());

    off64_t primal_offset = *delete_queue::primal_offset(queue_root_buf);
    large_buf_ref *t_o_ref = delete_queue::timestamps_and_offsets_largebuf(queue_root_buf);
    large_buf_ref *keys_ref = delete_queue::keys_largebuf(queue_root_buf);

    if (t_o_ref->size != 0 && keys_ref->size != 0) {

        rassert(t_o_ref->size % sizeof(delete_queue::t_and_o) == 0);

        // TODO: DON'T hold the queue_root lock for the entire operation.  Sheesh.

        int64_t begin_offset = 0, end_offset = 0;

        {
            boost::scoped_ptr<large_buf_t> t_o_largebuf(new large_buf_t(txor, t_o_ref, lbref_limit_t(delete_queue::TIMESTAMPS_AND_OFFSETS_SIZE), rwi_read));
            co_acquire_large_buf(saver, t_o_largebuf.get());

            delete_queue::t_and_o tao;
            int64_t i = 0, ie = t_o_ref->size;
            bool begin_found = false, end_found = false;
            while (i < ie) {
                t_o_largebuf->read_at(i, &tao, sizeof(tao));
                if (!begin_found && begin_timestamp.time <= tao.timestamp.time) {
                    begin_offset = tao.offset - primal_offset;
                    begin_found = true;
                }
                if (end_timestamp.time <= tao.timestamp.time) {
                    rassert(begin_found);
                    end_offset = tao.offset - primal_offset;
                    end_found = true;
                    break;
                }
                i += sizeof(tao);
            }

            if (!begin_found) {
                goto done;
                // Nothing to do!
            }

            if (!end_found) {
                end_offset = keys_ref->size;
            }

            // So we have a begin_offset and an end_offset;
        }

        rassert(begin_offset <= end_offset);

        if (begin_offset < end_offset) {
            boost::scoped_ptr<large_buf_t> keys_largebuf(new large_buf_t(txor, keys_ref, lbref_limit_t(delete_queue::keys_largebuf_ref_size((*txor)->cache->get_block_size())), rwi_read));

            // TODO: acquire subinterval.
            co_acquire_large_buf(saver, keys_largebuf.get());

            int64_t n = end_offset - begin_offset;

            // TODO: don't copy needlessly... sheesh.  This is a fake
            // implementation, make something that actually streams later.
            scoped_malloc<byte> buf(n);

            keys_largebuf->read_at(begin_offset, buf.get(), n);

            byte *p = buf.get();
            byte *e = p + n;
            while (p < e) {
                store_key_t *k = reinterpret_cast<store_key_t *>(p);
                rassert(k->size + 1 <= e - p);

                recipient->deletion_key(k);
                p += k->size + 1;
            }
        }
    }

 done:
    recipient->done_deletion_keys();
}

// TODO: maybe this function should be somewhere else.  Well,
// certainly.  Right now we don't have a notion of an "empty"
// largebuf, so we'll know that we have to ->allocate the largebuf
// when we see a size of 0 in the large_buf_ref.
void initialize_large_buf_ref(large_buf_ref *ref, int size_in_bytes) {
    int ids_bytes = size_in_bytes - offsetof(large_buf_ref, block_ids);
    rassert(ids_bytes > 0);

    ref->offset = 0;
    ref->size = 0;
    for (int i = 0, e = ids_bytes / sizeof(block_id_t); i < e; ++i) {
        ref->block_ids[i] = NULL_BLOCK_ID;
    }
}

void initialize_empty_delete_queue(delete_queue_block_t *dqb, block_size_t block_size) {
    dqb->magic = delete_queue_block_t::expected_magic;
    *delete_queue::primal_offset(dqb) = 0;
    large_buf_ref *t_and_o = delete_queue::timestamps_and_offsets_largebuf(dqb);
    initialize_large_buf_ref(t_and_o, delete_queue::TIMESTAMPS_AND_OFFSETS_SIZE);
    large_buf_ref *k = delete_queue::keys_largebuf(dqb);
    initialize_large_buf_ref(k, delete_queue::keys_largebuf_ref_size(block_size));
}



const block_magic_t delete_queue_block_t::expected_magic = { { 'D', 'e', 'l', 'Q' } };



}  // namespace replication