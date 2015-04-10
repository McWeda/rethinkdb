// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/error.hpp"

#include "backtrace.hpp"
#include "containers/archive/stl_types.hpp"
#include "rdb_protocol/datum.hpp"
#include "rdb_protocol/term_walker.hpp"
#include "rdb_protocol/val.hpp"

namespace ql {

#ifdef RQL_ERROR_BT
#define RQL_ERROR_VAR
#else
#define RQL_ERROR_VAR UNUSED
#endif

void runtime_fail(base_exc_t::type_t type,
                  RQL_ERROR_VAR const char *test, RQL_ERROR_VAR const char *file,
                  RQL_ERROR_VAR int line,
                  std::string msg, backtrace_id_t bt_src) {
#ifdef RQL_ERROR_BT
    msg = strprintf("%s\nFailed assertion: %s\nAt: %s:%d",
                    msg.c_str(), test, file, line);
#endif
    throw exc_t(type, msg, bt_src);
}
void runtime_fail(base_exc_t::type_t type,
                  RQL_ERROR_VAR const char *test, RQL_ERROR_VAR const char *file,
                  RQL_ERROR_VAR int line, std::string msg) {
#ifdef RQL_ERROR_BT
    msg = strprintf("%s\nFailed assertion: %s\nAt: %s:%d",
                    msg.c_str(), test, file, line);
#endif
    throw datum_exc_t(type, msg);
}

void runtime_sanity_check_failed(const char *file, int line, const char *test,
                                 const std::string &msg) {
    lazy_backtrace_formatter_t bt;
    std::string error_msg = "[" + std::string(test) + "]";
    if (!msg.empty()) {
        error_msg += " " + msg;
    }
    throw exc_t(base_exc_t::GENERIC,
                strprintf("SANITY CHECK FAILED: %s at `%s:%d` (server is buggy).  "
                          "Backtrace:\n%s",
                          error_msg.c_str(), file, line, bt.addrs().c_str()),
                backtrace_id_t());
}

base_exc_t::type_t exc_type(const datum_t *d) {
    r_sanity_check(d);
    return d->get_type() == datum_t::R_NULL
        ? base_exc_t::NON_EXISTENCE
        : base_exc_t::GENERIC;
}
base_exc_t::type_t exc_type(const datum_t &d) {
    r_sanity_check(d.has());
    return exc_type(&d);
}
base_exc_t::type_t exc_type(const val_t *v) {
    r_sanity_check(v);
    if (v->get_type().is_convertible(val_t::type_t::DATUM)) {
        return exc_type(v->as_datum());
    } else {
        return base_exc_t::GENERIC;
    }
}
base_exc_t::type_t exc_type(const scoped_ptr_t<val_t> &v) {
    r_sanity_check(v.has());
    return exc_type(v.get());
}

backtrace_registry_t::backtrace_registry_t() {
    frames.emplace_back(0, datum_t::null());
}

backtrace_id_t backtrace_registry_t::new_frame(backtrace_id_t parent_bt, 
                                               int32_t arg) {
    frames.emplace_back(parent_bt, datum_t(arg));
}

backtrace_id_t backtrace_registry_t::new_frame(backtrace_id_t parent_bt,
                                               const std::string &optarg) {
    frames.emplace_back(parent_bt, datum_t(optarg));
}

backtrace_id_t backtrace_registry_t::new_frame(backtrace_id_t parent_bt,
                                               const char *optarg) {
    frames.emplace_back(parent_bt, datum_t(optarg));
}

datum_t get_backtrace(backtrace_id_t bt, size_t dummy_frames) {
    r_sanity_check(bt < frames.size());
    datum_array_builder_t builder(configured_limits_t::unlimited);
    for (const frame_t &f = frames[bt]; !f.is_head(); f = frames[f.parent]) {
        r_sanity_check(f.parent < frames.size());
        if (dummy_frames > 0) {
            --dummy_frames;
        } else {
            builder.add(f.val);
        }
    }
    return std::move(builder).to_datum();
}

void fill_backtrace(Backtrace *bt_out,
                    ql::datum_t backtrace) {
    for (size_t i = 0; i < bt.arr_size(); ++i) {
        datum_t frame = bt.get(i);
        Frame *pb_frame = bt_out->add_frames();
        if (frame.get_type() == datum_t::type_t::R_STR) {
            pb_frame->set_type(Frame::FrameType::OPT);
            pb_frame->set_opt(frame.as_str().to_std());
        } else if (frame.get_type() == datum_t::type_t::R_NUM) {
            pb_frame->set_type(Frame::FrameType::POS);
            pb_frame->set_pos(frame.as_int());
        } else {
            unreachable();
        }
    }
}

void fill_error(Response *res,
                Response::ResponseType type,
                const char *message,
                datum_t backtrace) {
    guarantee(type == Response::CLIENT_ERROR ||
              type == Response::COMPILE_ERROR ||
              type == Response::RUNTIME_ERROR);
    Datum error_msg;
    error_msg.set_type(Datum::R_STR);
    error_msg.set_r_str(message);
    res->set_type(type);
    res->clear_response();
    res->clear_profile();
    *res->add_response() = error_msg;
    fill_backtrace(res->mutable_backtrace(), backtrace);
}

RDB_IMPL_SERIALIZABLE_3_SINCE_v1_13(exc_t, type_, msg, bt, dummy_frames);
RDB_IMPL_SERIALIZABLE_2_SINCE_v1_13(datum_exc_t, type_, msg);


} // namespace ql
