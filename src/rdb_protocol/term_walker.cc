// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "rdb_protocol/term_walker.hpp"

#include "rdb_protocol/error.hpp"
#include "rdb_protocol/minidriver.hpp"
#include "rdb_protocol/pseudo_time.hpp"
#include "rdb_protocol/ql2.pb.h"
#include "rdb_protocol/ql2_extensions.pb.h"

namespace ql {

// We use this class to walk a term and do something to every node.
class term_walker_t {
public:
    // This constructor checks that the term-tree is well-formed.
    explicit term_walker_t(Term *root) {
        frame_t frame(&frames, root->type(), datum_t::null());
        walk(root);
    }

    ~term_walker_t() {
        r_sanity_check(frames.empty());
    }

    // Build up an intrusive list stack for reporting backtraces
    // without compiling the terms or using much dynamic memory.
    class frame_t : public intrusive_list_node_t<frame_t> {
    public:
        frame_t(intrusive_list_t<frame_t> *_parent_list,
                Term::TermType _term_type,
                datum_t _val) :
            parent_list(_parent_list),
            term_type(_term_type),
            val(_val),
            writes_legal(true)
        {
            // TODO, RSI: The is_stream_funcall_frame crap didn't make any sense
            frame_t *prev_frame = parent_list->tail();
            if (prev_frame != nullptr) {
                writes_legal = !term_forbids_writes(term_type) &&
                               prev_frame->writes_legal;
            }
            parent_list->push_back(this);
        }

        ~frame_t() {
            parent_list->remove(this);
        }

        intrusive_list_t<frame_t> *parent_list;
        const Term::TermType term_type;
        const datum_t val;
        bool writes_legal;
    };

    void walk(Term *t) {
        frame_t *this_frame = frames.tail();
        guarantee(this_frame != nullptr);

        if (t->type() == Term::NOW && t->args_size() == 0) {
            // Construct curtime the first time we access it
            if (!curtime.has()) {
                curtime = pseudo::time_now();
            }
            *t = r::expr(curtime).get();
        }

        if (t->type() == Term::ASC || t->type() == Term::DESC) {
            frame_t *last_frame = frames.prev(this_frame);
            if (last_frame != nullptr && last_frame->term_type != Term::ORDER_BY) {
                throw term_walker_exc_t(strprintf("%s may only be used as an argument "
                    "to ORDER_BY.", (t->type() == Term::ASC ? "ASC" : "DESC")),
                    backtrace());
            }
        }

        if (term_is_write_or_meta(t->type()) && !this_frame->writes_legal) {
            throw term_walker_exc_t(strprintf("Cannot nest writes or meta ops in "
                "stream operations.  Use FOR_EACH instead."), backtrace());
        }

        for (int i = 0; i < t->args_size(); ++i) {
            Term *child = t->mutable_args(i);
            frame_t frame(&frames, child->type(), datum_t(static_cast<double>(i)));
            walk(child);
        }
        for (int i = 0; i < t->optargs_size(); ++i) {
            Term_AssocPair *ap = t->mutable_optargs(i);
            Term *child = ap->mutable_val();
            frame_t frame(&frames, child->type(), datum_t(ap->key().c_str()));
            walk(child);
        }
    }
private:
    datum_t backtrace() {
        datum_array_builder_t builder(configured_limits_t::unlimited);
        for (frame_t *f = frames.tail(); f != nullptr; f = frames.prev(f)) {
            if (f->val.get_type() != datum_t::type_t::R_NULL) {
                builder.add(f->val);
            }
        }
        return std::move(builder).to_datum();
    }


    // Returns true if `t` is a write or a meta op.
    static bool term_is_write_or_meta(Term::TermType type) {
        switch (type) {
        case Term::UPDATE:
        case Term::DELETE:
        case Term::INSERT:
        case Term::REPLACE:
        case Term::DB_CREATE:
        case Term::DB_DROP:
        case Term::TABLE_CREATE:
        case Term::TABLE_DROP:
        case Term::WAIT:
        case Term::RECONFIGURE:
        case Term::REBALANCE:
        case Term::SYNC:
        case Term::INDEX_CREATE:
        case Term::INDEX_DROP:
        case Term::INDEX_WAIT:
        case Term::INDEX_RENAME:
            return true;

        case Term::DATUM:
        case Term::MAKE_ARRAY:
        case Term::MAKE_OBJ:
        case Term::BINARY:
        case Term::VAR:
        case Term::JAVASCRIPT:
        case Term::HTTP:
        case Term::ERROR:
        case Term::IMPLICIT_VAR:
        case Term::RANDOM:
        case Term::DB:
        case Term::TABLE:
        case Term::GET:
        case Term::GET_ALL:
        case Term::EQ:
        case Term::NE:
        case Term::LT:
        case Term::LE:
        case Term::GT:
        case Term::GE:
        case Term::NOT:
        case Term::ADD:
        case Term::SUB:
        case Term::MUL:
        case Term::DIV:
        case Term::MOD:
        case Term::APPEND:
        case Term::PREPEND:
        case Term::DIFFERENCE:
        case Term::SET_INSERT:
        case Term::SET_INTERSECTION:
        case Term::SET_UNION:
        case Term::SET_DIFFERENCE:
        case Term::SLICE:
        case Term::OFFSETS_OF:
        case Term::GET_FIELD:
        case Term::HAS_FIELDS:
        case Term::PLUCK:
        case Term::WITHOUT:
        case Term::MERGE:
        case Term::LITERAL:
        case Term::BETWEEN_DEPRECATED:
        case Term::BETWEEN:
        case Term::CHANGES:
        case Term::REDUCE:
        case Term::MAP:
        case Term::FILTER:
        case Term::CONCAT_MAP:
        case Term::GROUP:
        case Term::ORDER_BY:
        case Term::DISTINCT:
        case Term::COUNT:
        case Term::SUM:
        case Term::AVG:
        case Term::MIN:
        case Term::MAX:
        case Term::UNION:
        case Term::NTH:
        case Term::BRACKET:
        case Term::ARGS:
        case Term::LIMIT:
        case Term::SKIP:
        case Term::INNER_JOIN:
        case Term::OUTER_JOIN:
        case Term::EQ_JOIN:
        case Term::ZIP:
        case Term::RANGE:
        case Term::INSERT_AT:
        case Term::DELETE_AT:
        case Term::CHANGE_AT:
        case Term::SPLICE_AT:
        case Term::COERCE_TO:
        case Term::UNGROUP:
        case Term::TYPE_OF:
        case Term::FUNCALL:
        case Term::BRANCH:
        case Term::OR:
        case Term::AND:
        case Term::FOR_EACH:
        case Term::FUNC:
        case Term::ASC:
        case Term::DESC:
        case Term::INFO:
        case Term::MATCH:
        case Term::SPLIT:
        case Term::UPCASE:
        case Term::DOWNCASE:
        case Term::SAMPLE:
        case Term::IS_EMPTY:
        case Term::DEFAULT:
        case Term::CONTAINS:
        case Term::KEYS:
        case Term::OBJECT:
        case Term::WITH_FIELDS:
        case Term::JSON:
        case Term::TO_JSON_STRING:
        case Term::ISO8601:
        case Term::TO_ISO8601:
        case Term::EPOCH_TIME:
        case Term::TO_EPOCH_TIME:
        case Term::NOW:
        case Term::IN_TIMEZONE:
        case Term::DURING:
        case Term::DATE:
        case Term::TIME_OF_DAY:
        case Term::TIMEZONE:
        case Term::TIME:
        case Term::YEAR:
        case Term::MONTH:
        case Term::DAY:
        case Term::DAY_OF_WEEK:
        case Term::DAY_OF_YEAR:
        case Term::HOURS:
        case Term::MINUTES:
        case Term::SECONDS:
        case Term::MONDAY:
        case Term::TUESDAY:
        case Term::WEDNESDAY:
        case Term::THURSDAY:
        case Term::FRIDAY:
        case Term::SATURDAY:
        case Term::SUNDAY:
        case Term::JANUARY:
        case Term::FEBRUARY:
        case Term::MARCH:
        case Term::APRIL:
        case Term::MAY:
        case Term::JUNE:
        case Term::JULY:
        case Term::AUGUST:
        case Term::SEPTEMBER:
        case Term::OCTOBER:
        case Term::NOVEMBER:
        case Term::DECEMBER:
        case Term::DB_LIST:
        case Term::TABLE_LIST:
        case Term::CONFIG:
        case Term::STATUS:
        case Term::INDEX_LIST:
        case Term::INDEX_STATUS:
        case Term::GEOJSON:
        case Term::TO_GEOJSON:
        case Term::POINT:
        case Term::LINE:
        case Term::POLYGON:
        case Term::DISTANCE:
        case Term::INTERSECTS:
        case Term::INCLUDES:
        case Term::CIRCLE:
        case Term::GET_INTERSECTING:
        case Term::FILL:
        case Term::GET_NEAREST:
        case Term::UUID:
        case Term::POLYGON_SUB:
        case Term::MINVAL:
        case Term::MAXVAL:
        case Term::FLOOR:
        case Term::CEIL:
        case Term::ROUND:
            return false;
        default: unreachable();
        }
    }

    static bool term_forbids_writes(Term::TermType type) {
        switch (type) {
        case Term::REDUCE:
        case Term::MAP:
        case Term::FILTER:
        case Term::CONCAT_MAP:
        case Term::GROUP:
        case Term::INNER_JOIN:
        case Term::OUTER_JOIN:
        case Term::EQ_JOIN:
        case Term::UPDATE:
        case Term::DELETE:
        case Term::REPLACE:
        case Term::INSERT:
        case Term::COUNT:
        case Term::SUM:
        case Term::AVG:
        case Term::MIN:
        case Term::MAX:
            return true;

        case Term::DATUM:
        case Term::MAKE_ARRAY:
        case Term::MAKE_OBJ:
        case Term::BINARY:
        case Term::VAR:
        case Term::JAVASCRIPT:
        case Term::HTTP:
        case Term::ERROR:
        case Term::IMPLICIT_VAR:
        case Term::RANDOM:
        case Term::DB:
        case Term::TABLE:
        case Term::GET:
        case Term::GET_ALL:
        case Term::EQ:
        case Term::NE:
        case Term::LT:
        case Term::LE:
        case Term::GT:
        case Term::GE:
        case Term::NOT:
        case Term::ADD:
        case Term::SUB:
        case Term::MUL:
        case Term::DIV:
        case Term::MOD:
        case Term::APPEND:
        case Term::PREPEND:
        case Term::DIFFERENCE:
        case Term::SET_INSERT:
        case Term::SET_INTERSECTION:
        case Term::SET_UNION:
        case Term::SET_DIFFERENCE:
        case Term::SLICE:
        case Term::OFFSETS_OF:
        case Term::GET_FIELD:
        case Term::HAS_FIELDS:
        case Term::PLUCK:
        case Term::WITHOUT:
        case Term::MERGE:
        case Term::ARGS:
        case Term::LITERAL:
        case Term::BETWEEN_DEPRECATED:
        case Term::BETWEEN:
        case Term::CHANGES:
        case Term::ORDER_BY:
        case Term::DISTINCT:
        case Term::UNION:
        case Term::NTH:
        case Term::BRACKET:
        case Term::LIMIT:
        case Term::SKIP:
        case Term::ZIP:
        case Term::RANGE:
        case Term::INSERT_AT:
        case Term::DELETE_AT:
        case Term::CHANGE_AT:
        case Term::SPLICE_AT:
        case Term::COERCE_TO:
        case Term::UNGROUP:
        case Term::TYPE_OF:
        case Term::DB_CREATE:
        case Term::DB_DROP:
        case Term::DB_LIST:
        case Term::TABLE_CREATE:
        case Term::TABLE_DROP:
        case Term::TABLE_LIST:
        case Term::CONFIG:
        case Term::STATUS:
        case Term::WAIT:
        case Term::RECONFIGURE:
        case Term::REBALANCE:
        case Term::SYNC:
        case Term::INDEX_CREATE:
        case Term::INDEX_DROP:
        case Term::INDEX_LIST:
        case Term::INDEX_STATUS:
        case Term::INDEX_WAIT:
        case Term::INDEX_RENAME:
        case Term::FUNCALL:
        case Term::BRANCH:
        case Term::OR:
        case Term::AND:
        case Term::FOR_EACH:
        case Term::FUNC:
        case Term::ASC:
        case Term::DESC:
        case Term::INFO:
        case Term::MATCH:
        case Term::SPLIT:
        case Term::UPCASE:
        case Term::DOWNCASE:
        case Term::SAMPLE:
        case Term::IS_EMPTY:
        case Term::DEFAULT:
        case Term::CONTAINS:
        case Term::KEYS:
        case Term::OBJECT:
        case Term::WITH_FIELDS:
        case Term::JSON:
        case Term::TO_JSON_STRING:
        case Term::ISO8601:
        case Term::TO_ISO8601:
        case Term::EPOCH_TIME:
        case Term::TO_EPOCH_TIME:
        case Term::NOW:
        case Term::IN_TIMEZONE:
        case Term::DURING:
        case Term::DATE:
        case Term::TIME_OF_DAY:
        case Term::TIMEZONE:
        case Term::TIME:
        case Term::YEAR:
        case Term::MONTH:
        case Term::DAY:
        case Term::DAY_OF_WEEK:
        case Term::DAY_OF_YEAR:
        case Term::HOURS:
        case Term::MINUTES:
        case Term::SECONDS:
        case Term::MONDAY:
        case Term::TUESDAY:
        case Term::WEDNESDAY:
        case Term::THURSDAY:
        case Term::FRIDAY:
        case Term::SATURDAY:
        case Term::SUNDAY:
        case Term::JANUARY:
        case Term::FEBRUARY:
        case Term::MARCH:
        case Term::APRIL:
        case Term::MAY:
        case Term::JUNE:
        case Term::JULY:
        case Term::AUGUST:
        case Term::SEPTEMBER:
        case Term::OCTOBER:
        case Term::NOVEMBER:
        case Term::DECEMBER:
        case Term::GEOJSON:
        case Term::TO_GEOJSON:
        case Term::POINT:
        case Term::LINE:
        case Term::POLYGON:
        case Term::DISTANCE:
        case Term::INTERSECTS:
        case Term::INCLUDES:
        case Term::CIRCLE:
        case Term::GET_INTERSECTING:
        case Term::FILL:
        case Term::GET_NEAREST:
        case Term::UUID:
        case Term::POLYGON_SUB:
        case Term::MINVAL:
        case Term::MAXVAL:
        case Term::FLOOR:
        case Term::CEIL:
        case Term::ROUND:
            return false;
        default: unreachable();
        }
    }

    datum_t curtime;
    intrusive_list_t<frame_t> frames;
};

void preprocess_term(Term *root) {
    term_walker_t walker(root);
}

}  // namespace ql
