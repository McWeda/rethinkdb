#ifndef RDB_PROTOCOL_TERM_WALKER_HPP_
#define RDB_PROTOCOL_TERM_WALKER_HPP_

#include <stdexcept>
#include <string>

#include "rdb_protocol/datum.hpp"

class Term;

namespace ql {

class term_walker_exc_t : public std::exception {
public:
    term_walker_exc_t(const std::string &_message,
                      const datum_t &_bt) :
        message(_message), bt(_bt) { }
    virtual ~term_walker_exc_t() { };

    const char *what() const throw () {
        return message.c_str();
    }

    datum_t backtrace() const {
        return bt;
    }

private:
    const std::string message;
    const datum_t bt;
};

// Fills in the backtraces of a term and checks that it's well-formed with
// regard to write placement.
void preprocess_term(Term *root);

} // namespace ql

#endif // RDB_PROTOCOL_TERM_WALKER_HPP_
