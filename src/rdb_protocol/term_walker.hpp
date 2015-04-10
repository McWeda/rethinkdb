#ifndef RDB_PROTOCOL_TERM_WALKER_HPP_
#define RDB_PROTOCOL_TERM_WALKER_HPP_

class Term;

namespace ql {

// Fills in the backtraces of a term and checks that it's well-formed with
// regard to write placement.
void preprocess_term(Term *root);

} // namespace ql

#endif // RDB_PROTOCOL_TERM_WALKER_HPP_
