#ifndef RDB_PROTOCOL_QUERY_LANGUAGE_HPP_
#define RDB_PROTOCOL_QUERY_LANGUAGE_HPP_

#include <list>
#include <deque>

#include "utils.hpp"
#include <boost/variant.hpp>
#include <boost/shared_ptr.hpp>

#include "clustering/administration/metadata.hpp"
#include "clustering/administration/namespace_interface_repository.hpp"
#include "extproc/pool.hpp"
#include "http/json.hpp"
#include "rdb_protocol/backtrace.hpp"
#include "rdb_protocol/protocol.hpp"
#include "rdb_protocol/query_language.pb.h"

namespace query_language {

/* `bad_protobuf_exc_t` is thrown if the client sends us a protocol buffer that
doesn't match our schema. This should only happen if the client itself is
broken. */

class bad_protobuf_exc_t : public std::exception {
public:
    ~bad_protobuf_exc_t() throw () { }

    const char *what() const throw () {
        return "bad protocol buffer";
    }
};

/* `bad_query_exc_t` is thrown if the user writes a query that accesses
undefined variables or that has mismatched types. The difference between this
and `bad_protobuf_exc_t` is that `bad_protobuf_exc_t` is the client's fault and
`bad_query_exc_t` is the client's user's fault. */

class bad_query_exc_t : public std::exception {
public:
    explicit bad_query_exc_t(const std::string &s, const backtrace_t &bt) : message(s), backtrace(bt) { }

    ~bad_query_exc_t() throw () { }

    const char *what() const throw () {
        return message.c_str();
    }

    std::string message;
    backtrace_t backtrace;
};

enum term_type_t {
    TERM_TYPE_JSON,
    TERM_TYPE_STREAM,
    TERM_TYPE_VIEW,

    /* This is the type of `Error` terms. It's called "arbitrary" because an
    `Error` term can be either a stream or an object. It is a subtype of every
    type. */
    TERM_TYPE_ARBITRARY
};

class function_type_t {
public:
    // _n_args==-1 indicates a variadic function
    function_type_t(const term_type_t &_return_type);
    function_type_t(const term_type_t& _arg_type, int _n_args, const term_type_t& _return_type);
    function_type_t(const term_type_t& _arg1_type, const term_type_t& _arg2_type, const term_type_t& _return_type);

    const term_type_t& get_arg_type(int n) const;
    const term_type_t& get_return_type() const;
    bool is_variadic() const;

    int get_n_args() const;
private:
    term_type_t arg_type[3];
    int n_args;
    term_type_t return_type;
};

template <class T>
class variable_scope_t {
public:
    void put_in_scope(const std::string &name, const T &t) {
        rassert(!scopes.empty());
        scopes.front()[name] = t;
    }

    T get(const std::string &name) {
        for (typename scopes_t::iterator it  = scopes.begin();
                                         it != scopes.end();
                                         ++it) {
            typename std::map<std::string, T>::iterator jt = it->find(name);
            if (jt != it->end()) {
                return jt->second;
            }
        }

        unreachable("Variable not in scope, probably because the code fails to call is_in_scope().");
    }

    // Calling this only makes sense in the typechecker. All variables
    // are guranteed by the typechecker to be present at runtime.
    bool is_in_scope(const std::string &name) {
        for (typename scopes_t::iterator it  = scopes.begin();
                                         it != scopes.end();
                                         ++it) {
            typename std::map<std::string, T>::iterator jt = it->find(name);
            if (jt != it->end()) {
                return true;
            }
        }
        return false;
    }

    void push() {
        scopes.push_front(std::map<std::string, T>());
    }

    void pop() {
        scopes.pop_front();
    }

    // TODO (rntz): find a better way to do this.
    void dump(std::map<std::string, T> *map) {
        map->clear();
        for (typename std::deque<std::map<std::string, T> >::iterator sit = scopes.begin(); sit != scopes.end(); ++sit) {
            for (typename std::map<std::string, T>::iterator it = sit->begin(); it != sit->end(); ++it) {
                // Earlier bindings take precedence over later ones.
                if (!map->count(it->first))
                    map->insert(*it);
            }
        }
    }

    struct new_scope_t {
        explicit new_scope_t(variable_scope_t<T> *_parent)
            : parent(_parent)
        {
            parent->push();
        }
        ~new_scope_t() {
            parent->pop();
        }

        variable_scope_t<T> *parent;
    };
private:
    typedef std::deque<std::map<std::string, T> > scopes_t;
    scopes_t scopes;
};

typedef variable_scope_t<term_type_t> variable_type_scope_t;

typedef variable_type_scope_t::new_scope_t new_scope_t;

/* an implicit_value_t allows for a specific implicit value to exist at certain
 * points in execution for example the argument to get attr is implicitly
 * defined to be the value of the row upon entering a filter,map etc.
 * implicit_value_t supports scopes for its values but does not allow looking
 * up values in any scope to the current one. */
template <class T>
class implicit_value_t {
public:
    implicit_value_t() { 
        push();
    }

    void push() {
        scopes.push_front(boost::optional<T>());
    }

    void push(const T &t) {
        scopes.push_front(t);
    }

    void pop() {
        scopes.pop_front();
    }

    class impliciter_t {
    public:
        explicit impliciter_t(implicit_value_t *_parent) 
            : parent(_parent)
        {
            parent->push();
        }

        impliciter_t(implicit_value_t *_parent, const T& t) 
            : parent(_parent)
        {
            parent->push(t);
        }

        ~impliciter_t() {
            parent->pop();
        }
    private:
        implicit_value_t *parent;
    };

    bool has_value() {
        return scopes.front();
    }

    T get_value() {
        return *scopes.front();
    }

private:
    typedef std::deque<boost::optional<T> > scopes_t;
    scopes_t scopes;
};

typedef implicit_value_t<term_type_t> implicit_type_t;

struct type_checking_environment_t {
    variable_type_scope_t scope;
    implicit_type_t implicit_type;
};

/* These functions throw exceptions if their inputs aren't well defined or
fail type-checking. (A well-defined input has the correct fields filled in.) */

term_type_t get_term_type(const Term &t, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_term_type(const Term &t, term_type_t expected, type_checking_environment_t *env, const backtrace_t &backtrace);
function_type_t get_function_type(const Builtin &b, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_reduction_type(const Reduction &m, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_mapping_type(const Mapping &m, term_type_t return_type, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_predicate_type(const Predicate &m, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_read_query_type(const ReadQuery &rq, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_write_query_type(const WriteQuery &wq, type_checking_environment_t *env, const backtrace_t &backtrace);
void check_query_type(const Query &q, type_checking_environment_t *env, const backtrace_t &backtrace);

/* functions to evaluate the queries */

typedef std::list<boost::shared_ptr<scoped_cJSON_t> > cJSON_list_t;

class json_stream_t {
public:
    virtual boost::shared_ptr<scoped_cJSON_t> next() = 0;
    virtual ~json_stream_t() { }
};

class in_memory_stream_t : public json_stream_t {
public:
    template <class iterator>
    in_memory_stream_t(const iterator &begin, const iterator &end)
        : data(begin, end)
    { }

    in_memory_stream_t(json_array_iterator_t it) {
        while (cJSON *json = it.next()) {
            data.push_back(boost::shared_ptr<scoped_cJSON_t>(new scoped_cJSON_t(cJSON_DeepCopy(json))));
        }
    }

    in_memory_stream_t(boost::shared_ptr<json_stream_t> stream) {
        while (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
            data.push_back(json);
        }
    }

    template <class Ordering>
    void sort(const Ordering &o) {
        data.sort(o);
    }

    boost::shared_ptr<scoped_cJSON_t> next() {
        if (data.empty()) {
            return boost::shared_ptr<scoped_cJSON_t>();
        } else {
            boost::shared_ptr<scoped_cJSON_t> res = data.front();
            data.pop_front();
            return res;
        }
    }
private:
    cJSON_list_t data;
};

class stream_multiplexer_t {
public:

    stream_multiplexer_t() { }
    explicit stream_multiplexer_t(boost::shared_ptr<json_stream_t> _stream)
        : stream(_stream)
    { }

    typedef std::vector<boost::shared_ptr<scoped_cJSON_t> > cJSON_vector_t;

    class stream_t : public json_stream_t {
    public:
        stream_t(boost::shared_ptr<stream_multiplexer_t> _parent)
            : parent(_parent), index(0)
        {
            rassert(parent->stream);
        }

        boost::shared_ptr<scoped_cJSON_t> next() {
            while (index >= parent->data.size()) {
                if (!parent->maybe_read_more()) {
                    return boost::shared_ptr<scoped_cJSON_t>();
                }
            }

            return parent->data[index++];
        }
    private:
        boost::shared_ptr<stream_multiplexer_t> parent;
        cJSON_vector_t::size_type index;
    };

private:
    friend class stream_t;

    bool maybe_read_more() {
        if (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
            data.push_back(json);
            return true;
        } else {
            return false;
        }
    }

    //TODO this should probably not be a vector
    boost::shared_ptr<json_stream_t> stream;

    cJSON_vector_t data;
};

class union_stream_t : public json_stream_t {
public:
    typedef std::list<boost::shared_ptr<json_stream_t> > stream_list_t;

    union_stream_t(const stream_list_t &_streams)
        : streams(_streams), hd(streams.begin())
    { }

    boost::shared_ptr<scoped_cJSON_t> next() {
        while (hd != streams.end()) {
            if (boost::shared_ptr<scoped_cJSON_t> json = (*hd)->next()) {
                return json;
            } else {
                ++hd;
            }
        }
        return boost::shared_ptr<scoped_cJSON_t>();
    }

private:
    stream_list_t streams;
    stream_list_t::iterator hd;
};

template <class P>
class filter_stream_t : public json_stream_t {
public:
    typedef boost::function<bool(boost::shared_ptr<scoped_cJSON_t>)> predicate;
    filter_stream_t(boost::shared_ptr<json_stream_t> _stream, const P &_p)
        : stream(_stream), p(_p)
    { }

    boost::shared_ptr<scoped_cJSON_t> next() {
        while (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
            if (p(json)) {
                return json;
            }
        }
        return boost::shared_ptr<scoped_cJSON_t>();
    }

private:
    boost::shared_ptr<json_stream_t> stream;
    P p;
};

template <class F>
class mapping_stream_t : public json_stream_t {
public:
    mapping_stream_t(boost::shared_ptr<json_stream_t> _stream, const F &_f)
        : stream(_stream), f(_f)
    { }

    boost::shared_ptr<scoped_cJSON_t> next() {
        if (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
            return f(json);
        } else {
            return json;
        }
    }

private:
    boost::shared_ptr<json_stream_t> stream;
    F f;
};

template <class F>
class concat_mapping_stream_t : public  json_stream_t {
public:
    concat_mapping_stream_t(boost::shared_ptr<json_stream_t> _stream, const F &_f)
        : stream(_stream), f(_f)
    {
        f = _f;
        if (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
            substream = f(json);
        }
    }

    boost::shared_ptr<scoped_cJSON_t> next() {
        boost::shared_ptr<scoped_cJSON_t> res;

        while (!res) {
            if (!substream) {
                return res;
            } else if ((res = substream->next())) {
                continue;
            } else if (boost::shared_ptr<scoped_cJSON_t> json = stream->next()) {
                substream = f(json);
            } else {
                substream.reset();
            }
        }

        return res;
    }

private:
    boost::shared_ptr<json_stream_t> stream, substream;
    F f;
};

class limit_stream_t : public json_stream_t {
public:
    limit_stream_t(boost::shared_ptr<json_stream_t> _stream, int _limit)
        : stream(_stream), limit(_limit)
    {
        guarantee(limit >= 0);
    }

    boost::shared_ptr<scoped_cJSON_t> next() {
        if (limit == 0) {
            return boost::shared_ptr<scoped_cJSON_t>();
        } else {
            limit--;
            return stream->next();
        }
    }

private:
    boost::shared_ptr<json_stream_t> stream;
    int limit;
};

//Scopes for single pieces of json
typedef variable_scope_t<boost::shared_ptr<scoped_cJSON_t> > variable_val_scope_t;

typedef variable_val_scope_t::new_scope_t new_val_scope_t;

//scopes for json streams
typedef variable_scope_t<boost::shared_ptr<stream_multiplexer_t> > variable_stream_scope_t;

typedef variable_stream_scope_t::new_scope_t new_stream_scope_t;

class runtime_exc_t {
public:
    runtime_exc_t(const std::string &_what, const backtrace_t &bt)
        : message(_what), backtrace(bt)
    { }

    std::string what() const throw() {
        return message;
    }

    std::string message;
    backtrace_t backtrace;
};

class runtime_environment_t {
public:
    runtime_environment_t(extproc::pool_group_t *_pool_group,
                          namespace_repo_t<rdb_protocol_t> *_ns_repo,
                          boost::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t> > _semilattice_metadata,
                          signal_t *_interruptor)
        : pool_group(_pool_group),
          ns_repo(_ns_repo),
          semilattice_metadata(_semilattice_metadata),
          interruptor(_interruptor)
    { }

    variable_val_scope_t scope;
    variable_stream_scope_t stream_scope;
    type_checking_environment_t type_env;

    implicit_value_t<boost::shared_ptr<scoped_cJSON_t> > implicit_attribute_value;

    extproc::pool_group_t *pool_group;   // for running external JS jobs
    namespace_repo_t<rdb_protocol_t> *ns_repo;
    //TODO this should really just be the namespace metadata... but
    //constructing views is too hard :-/
    boost::shared_ptr<semilattice_read_view_t<cluster_semilattice_metadata_t> > semilattice_metadata;
    signal_t *interruptor;
};

typedef implicit_value_t<boost::shared_ptr<scoped_cJSON_t> >::impliciter_t implicit_value_setter_t;

//TODO most of these functions that are supposed to only throw runtime exceptions

void execute(const Query &q, runtime_environment_t *, Response *res, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

void execute(const ReadQuery &r, runtime_environment_t *, Response *res, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

void execute(const WriteQuery &r, runtime_environment_t *, Response *res, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

boost::shared_ptr<scoped_cJSON_t> eval(const Term &t, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

boost::shared_ptr<json_stream_t> eval_stream(const Term &t, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

boost::shared_ptr<scoped_cJSON_t> eval(const Term::Call &c, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

boost::shared_ptr<json_stream_t> eval_stream(const Term::Call &c, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

boost::shared_ptr<scoped_cJSON_t> eval_cmp(const Term::Call &c, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

namespace_repo_t<rdb_protocol_t>::access_t eval(const TableRef &t, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

class view_t {
public:
    view_t(const namespace_repo_t<rdb_protocol_t>::access_t &_access,
           boost::shared_ptr<json_stream_t> _stream)
        : access(_access), stream(_stream) { }

    namespace_repo_t<rdb_protocol_t>::access_t access;
    boost::shared_ptr<json_stream_t> stream;
};

view_t eval_view(const Term::Table &t, runtime_environment_t *, const backtrace_t &backtrace) THROWS_ONLY(runtime_exc_t);

} //namespace query_language

#endif /* RDB_PROTOCOL_QUERY_LANGUAGE_HPP_ */
