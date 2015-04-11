// Copyright 2010-2015 RethinkDB, all rights reserved.
#ifndef RDB_PROTOCOL_BACKTRACE_HPP_
#define RDB_PROTOCOL_BACKTRACE_HPP_
#include <vector>

#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/error.hpp"
#include "rdb_protocol/ql2.pb.h"
#include "rdb_protocol/ql2_extensions.pb.h"

namespace ql {

class backtrace_registry_t {
public:
    virtual ~backtrace_registry_t() { }

    virtual backtrace_id_t new_frame(backtrace_id_t parent_bt,
                                     const datum_t &val) = 0;

    static const datum_t EMPTY_BACKTRACE;
};

// All backtrace_ids allocated through this object will be the same as the
// backtrace_id it was originally constructed with.  This is used when
// compiling functions or rewrites.
class dummy_backtrace_registry_t : public backtrace_registry_t {
public:
    dummy_backtrace_registry_t(backtrace_id_t _original_bt) :
        original_bt(_original_bt) { }
    virtual ~dummy_backtrace_registry_t() { }

    backtrace_id_t new_frame(backtrace_id_t parent_bt, const datum_t &val);

private:
    backtrace_id_t original_bt;
};

class real_backtrace_registry_t : public backtrace_registry_t {
public:
    real_backtrace_registry_t();
    real_backtrace_registry_t(real_backtrace_registry_t &&other) :
        frames(std::move(other.frames)) { }
    virtual ~real_backtrace_registry_t() { }

    backtrace_id_t new_frame(backtrace_id_t parent_bt, const datum_t &val);

    datum_t datum_backtrace(const exc_t &ex) const;

private:
    struct frame_t {
        frame_t(); // Only for creating the HEAD term
        frame_t(backtrace_id_t _parent, datum_t _val);
        bool is_head() const;

        backtrace_id_t parent;
        datum_t val;
    };

    std::vector<frame_t> frames;
    DISABLE_COPYING(real_backtrace_registry_t);
};

void fill_backtrace(Backtrace *bt_out,
                    datum_t backtrace);

void fill_error(Response *res_out,
                Response::ResponseType type,
                const std::string &message,
                datum_t backtrace);

} // namespace ql

#endif // RDB_PROTOCOL_BACKTRACE_HPP_
