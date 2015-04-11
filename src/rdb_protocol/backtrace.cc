#include "rdb_protocol/backtrace.hpp"

namespace ql {

const datum_t backtrace_registry_t::EMPTY_BACKTRACE = datum_t::empty_array();

backtrace_id_t dummy_backtrace_registry_t::new_frame(backtrace_id_t,
                                                     const datum_t &) {
    return original_bt;
}

real_backtrace_registry_t::frame_t::frame_t(backtrace_id_t _parent, datum_t _val) :
    parent(_parent), val(_val) { }

bool real_backtrace_registry_t::frame_t::is_head() const {
    return val.get_type() == datum_t::type_t::R_NULL;
}

real_backtrace_registry_t::real_backtrace_registry_t() {
    frames.emplace_back(EMPTY_BACKTRACE_ID, datum_t::null());
}

backtrace_id_t real_backtrace_registry_t::new_frame(backtrace_id_t parent_bt,
                                                    const datum_t &val) {
    frames.emplace_back(parent_bt, val);
    return frames.size() - 1;
}

datum_t real_backtrace_registry_t::datum_backtrace(const exc_t &ex) const {
    size_t dummy_frames = ex.dummy_frames();
    r_sanity_check(ex.backtrace() < frames.size());
    datum_array_builder_t builder(configured_limits_t::unlimited);
    for (const frame_t *f = &frames[ex.backtrace()];
         !f->is_head(); f = &frames[f->parent]) {
        r_sanity_check(f->parent < frames.size());
        if (dummy_frames > 0) {
            --dummy_frames;
        } else {
            builder.add(f->val);
        }
    }
    return std::move(builder).to_datum();
}

void fill_backtrace(Backtrace *bt_out,
                    ql::datum_t bt_datum) {
    for (size_t i = 0; i < bt_datum.arr_size(); ++i) {
        datum_t frame = bt_datum.get(i);
        Frame *pb_frame = bt_out->add_frames();
        if (frame.get_type() == datum_t::type_t::R_STR) {
            pb_frame->set_type(Frame::OPT);
            pb_frame->set_opt(frame.as_str().to_std());
        } else if (frame.get_type() == datum_t::type_t::R_NUM) {
            pb_frame->set_type(Frame::POS);
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

}
