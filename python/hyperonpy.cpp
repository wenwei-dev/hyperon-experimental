#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <hyperon/hyperon.h>

#include <optional>

namespace py = pybind11;

template<class T, size_t N>
constexpr size_t lenghtof(T (&)[N]) { return N; }

template <typename T>
struct CPtr {
    using type = T;
    CPtr(T* ptr) : ptr(ptr) {}
    T* ptr;
};

using CAtom = CPtr<atom_t>;
using CVecAtom = CPtr<vec_atom_t>;
using CBindings = CPtr<bindings_t>;
using CBindingsSet = CPtr<bindings_set_t>;
using CSpace = CPtr<space_t>;
using CTokenizer = CPtr<tokenizer_t>;
using CStepResult = CPtr<step_result_t>;
using CMetta = CPtr<metta_t>;

// Returns a string, created by executing a function that writes string data into a buffer
typedef size_t (*write_to_buf_func_t)(void*, char*, size_t);
std::string func_to_string(write_to_buf_func_t func, void* arg) {
    //First try with a 1K stack buffer, because that will work in the vast majority of cases
    char dst_buf[1024];
    size_t len = func(arg, dst_buf, 1024);
    if (len < 1024) {
        return std::string(dst_buf);
    } else {
        std::string new_string = std::string();
        new_string.resize(len+1); //Room for the terminator in the allocated buffer
        func(arg, new_string.data(), len+1);
        new_string.resize(len); //But the C++ string doesn't include the terminator in its len
        return new_string;
    }
}

static void copy_atoms(atom_array_t atoms, void* context) {
    py::list* list = static_cast<py::list*>(context);
    for (size_t i = 0; i < atoms.size; ++i) {
        list->append(CAtom(atom_clone(atoms.items[i])));
    }
}

static void copy_atom_to_dict(const var_atom_t* atom, void* context) {
    py::dict& pybindings = *static_cast<py::dict*>(context);
    pybindings[atom->var] = CAtom(atom->atom);
}

static void copy_lists_of_atom(atom_array_t atoms, void* context) {
    py::list* list_of_lists = static_cast<py::list*>(context);
    py::list list;
    copy_atoms(atoms, &list);
    list_of_lists->append(list);
}

py::object get_attr_or_fail(py::handle const& pyobj, char const* attr) {
    if (py::hasattr(pyobj, attr)) {
        return pyobj.attr(attr)();
    } else {
        std::string message = "Python object doesn't have a \"";
        message += attr;
        message += "\" attribute";
        throw std::runtime_error(message);
    }
}

extern "C" {
    exec_error_t *py_execute(const struct gnd_t* _gnd, struct vec_atom_t* args, struct vec_atom_t* ret);
    void py_match_(const struct gnd_t *_gnd, const struct atom_t *_atom, bindings_mut_callback_t callback, void *context);
    bool py_eq(const struct gnd_t* _a, const struct gnd_t* _b);
    struct gnd_t *py_clone(const struct gnd_t* _gnd);
    size_t py_display(const struct gnd_t* _gnd, char* buffer, size_t size);
    void py_free(struct gnd_t* _gnd);
}

const gnd_api_t PY_EXECUTABLE_MATCHABLE_API = { &py_execute, &py_match_, &py_eq, &py_clone, &py_display, &py_free };
const gnd_api_t PY_EXECUTABLE_API = { &py_execute, nullptr, &py_eq, &py_clone, &py_display, &py_free };
const gnd_api_t PY_MATCHABLE_API = { nullptr, &py_match_, &py_eq, &py_clone, &py_display, &py_free };
const gnd_api_t PY_VALUE_API = { nullptr, nullptr, &py_eq, &py_clone, &py_display, &py_free };

struct GroundedObject : gnd_t {
    GroundedObject(py::object pyobj, atom_t* typ) : pyobj(pyobj) {
        if (py::hasattr(pyobj, "execute") && py::hasattr(pyobj, "match_")) {
            this->api = &PY_EXECUTABLE_MATCHABLE_API;
        } else if (py::hasattr(pyobj, "execute")) {
            this->api = &PY_EXECUTABLE_API;
        } else if (py::hasattr(pyobj, "match_")) {
            this->api = &PY_MATCHABLE_API;
        } else {
            this->api = &PY_VALUE_API;
        }
        this->typ = typ;
    }
    virtual ~GroundedObject() {
        atom_free(this->typ);
    }
    py::object pyobj;
};

py::object inc_ref(py::object obj) {
    obj.inc_ref();
    return obj;
}

exec_error_t *py_execute(const struct gnd_t* _cgnd, struct vec_atom_t* _args, struct vec_atom_t* ret) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_execute_on_grounded_atom = hyperon.attr("call_execute_on_grounded_atom");
    py::handle NoReduceError = hyperon.attr("NoReduceError");
    py::object pyobj = static_cast<GroundedObject const*>(_cgnd)->pyobj;
    CAtom pytyp = static_cast<GroundedObject const*>(_cgnd)->typ;
    try {
        py::list args;
        for (size_t i = 0; i < vec_atom_size(_args); ++i) {
            args.append(CAtom(atom_clone(vec_atom_get(_args, i))));
        }
        py::list result = call_execute_on_grounded_atom(pyobj, pytyp, args);
        for (auto& atom:  result) {
            vec_atom_push(ret, atom_clone(atom.attr("catom").cast<CAtom>().ptr));
        }
        return nullptr;
    } catch (py::error_already_set &e) {
        if (e.matches(NoReduceError)) {
            return exec_error_no_reduce();
        } else {
            char message[4096];
            snprintf(message, lenghtof(message), "Exception caught:\n%s", e.what());
            return exec_error_runtime(message);
        }
    }
}

void py_match_(const struct gnd_t *_gnd, const struct atom_t *_atom, bindings_mut_callback_t callback, void *context) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_match_on_grounded_atom = hyperon.attr("call_match_on_grounded_atom");

    py::object pyobj = static_cast<GroundedObject const *>(_gnd)->pyobj;
    CAtom catom = atom_clone(_atom);
    py::list results = call_match_on_grounded_atom(pyobj, catom);

    for (py::handle result: results) {
        py::dict pybindings = result.cast<py::dict>();

        struct bindings_t* cbindings = bindings_new();
        for (auto var_atom : pybindings) {
            const std::string var  = var_atom.first.cast<py::str>();
            CAtom atom = atom_clone(var_atom.second.attr("catom").cast<CAtom>().ptr);
            var_atom_t varAtom{.var = var.c_str(), .atom = atom.ptr };

            bindings_add_var_binding(cbindings, &varAtom);
        }

        callback(cbindings, context);
    }
}

bool py_eq(const struct gnd_t* _a, const struct gnd_t* _b) {
    py::object a = static_cast<GroundedObject const*>(_a)->pyobj;
    py::object b = static_cast<GroundedObject const*>(_b)->pyobj;
    return a.equal(b);
}

struct gnd_t *py_clone(const struct gnd_t* _cgnd) {
    GroundedObject const* cgnd = static_cast<GroundedObject const*>(_cgnd);
    py::object pyobj = cgnd->pyobj;
    py::object copy = pyobj.attr("copy")();
    atom_t* typ = atom_clone(cgnd->typ);
    return new GroundedObject(copy, typ);
}

size_t py_display(const struct gnd_t* _cgnd, char* buffer, size_t size) {
    py::object pyobj = static_cast<GroundedObject const*>(_cgnd)->pyobj;
    std::string str = py::str(pyobj).cast<std::string>();
    strncpy(buffer, str.c_str(), size - 1);
    buffer[size - 1] = 0;
    return str.size();
}

void py_free(struct gnd_t* _cgnd) {
    delete static_cast<GroundedObject const*>(_cgnd);
}

extern "C" {
    bindings_set_t *py_space_query(const struct space_params_t *params, const struct atom_t *atom);
    vec_atom_t *py_space_subst(const struct space_params_t *params, const struct atom_t *pattern, const struct atom_t *tmpl);
    void py_space_add(const struct space_params_t *params, struct atom_t *atom);
    bool py_space_remove(const struct space_params_t *params, const struct atom_t *atom);
    bool py_space_replace(const struct space_params_t *params, const struct atom_t *from, struct atom_t *to);
    ssize_t py_space_atom_count(const struct space_params_t *params);
    void *py_space_new_atom_iter_state(const struct space_params_t *params);
    const atom_t *py_space_iter_next_atom(const struct space_params_t *params, void *state);
    void py_space_free_atom_iter_state(const struct space_params_t *params, void *state);
    void py_space_free_payload(void *payload);
}

const space_api_t PY_SPACE_NO_SUBST_API = {
    &py_space_query,
    NULL, //TODO: &py_space_subst
    &py_space_add,
    &py_space_remove,
    &py_space_replace,
    &py_space_atom_count,
    &py_space_new_atom_iter_state,
    &py_space_iter_next_atom,
    &py_space_free_atom_iter_state,
    &py_space_free_payload };

struct PySpace {
    PySpace(py::object pyobj) : pyobj(pyobj) {
    }
    virtual ~PySpace() {
    }
    py::object pyobj;
};

bindings_set_t *py_space_query(const struct space_params_t *params, const struct atom_t *query_atom) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_query_on_python_space = hyperon.attr("call_query_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    CAtom catom = atom_clone(query_atom);
    py::object result = call_query_on_python_space(pyobj, catom);
    const CBindingsSet &set = result.attr("c_set").cast<CBindingsSet>();
    return bindings_set_clone(set.ptr);
}

//TODO, currently Python spaces use the default subst implementation
// vec_atom_t *py_space_subst(const struct space_params_t *params, const struct atom_t *pattern, const struct atom_t *tmpl) {
//     //TODO
// }

void py_space_add(const struct space_params_t *params, struct atom_t *atom) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_add_on_python_space = hyperon.attr("call_add_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    atom_t* notify_atom = atom_clone(atom);
    CAtom catom = atom;
    call_add_on_python_space(pyobj, catom);

    //TODO: Create a mechanism so the Python code can do the notification manually, and bypass this
    // automatic notification code
    space_event_t* event = space_event_new_add(notify_atom);
    space_params_notify_all_observers(params, event);
    space_event_free(event);
}

bool py_space_remove(const struct space_params_t *params, const struct atom_t *atom) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_remove_on_python_space = hyperon.attr("call_remove_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    atom_t* notify_atom = atom_clone(atom);
    CAtom catom = atom_clone(atom);
    py::object result = call_remove_on_python_space(pyobj, catom);
    if (result.cast<bool>()) {
        //TODO: See comment about manual notification above
        space_event_t* event = space_event_new_remove(notify_atom);
        space_params_notify_all_observers(params, event);
        space_event_free(event);
        return true;
    } else {
        atom_free(notify_atom);
        return false;
    }
}

bool py_space_replace(const struct space_params_t *params, const struct atom_t *from, struct atom_t *to) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_replace_on_python_space = hyperon.attr("call_replace_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    atom_t* notify_from = atom_clone(from);
    atom_t* notify_to = atom_clone(to);
    CAtom catom_from = atom_clone(from);
    CAtom catom_to = to;
    py::object result = call_replace_on_python_space(pyobj, catom_from, catom_to);
    if (result.cast<bool>()) {
        //TODO: See comment about manual notification above
        space_event_t* event = space_event_new_replace(notify_from, notify_to);
        space_params_notify_all_observers(params, event);
        space_event_free(event);
        return true;
    } else {
        atom_free(notify_from);
        atom_free(notify_to);
        return false;
    }
}

ssize_t py_space_atom_count(const struct space_params_t *params) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_atom_count_on_python_space = hyperon.attr("call_atom_count_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    py::int_ result = call_atom_count_on_python_space(pyobj);
    return result.cast<ssize_t>();
}

void *py_space_new_atom_iter_state(const struct space_params_t *params) {
    py::object hyperon = py::module_::import("hyperon");
    py::function call_new_iter_state_on_python_space = hyperon.attr("call_new_iter_state_on_python_space");
    py::object pyobj = static_cast<PySpace const *>(params->payload)->pyobj;
    py::object result = call_new_iter_state_on_python_space(pyobj);
    if (result.is_none()) {
        return NULL;
    } else {
        py::function iter_init_fn = result.attr("__iter__");
        iter_init_fn();
        py::object* iter_buf = new py::object(result);
        return (void*)iter_buf;
    }
}

const atom_t *py_space_iter_next_atom(const struct space_params_t *params, void *state) {
    py::object* iter_buf = (py::object*)state;
    py::function next_fn = iter_buf->attr("__next__");
    try {
        py::object atom = next_fn();
        return atom.attr("catom").cast<CAtom>().ptr;
    } catch (pybind11::error_already_set &e) {
        if (e.matches(PyExc_StopIteration)) {
            return NULL;
        } else {
            throw;
        }
    }
}

void py_space_free_atom_iter_state(const struct space_params_t *params, void *state) {
    py::object* iter_buf = (py::object*)state;
    delete iter_buf;
}

void py_space_free_payload(void *payload) {
    delete static_cast<PySpace const*>(payload);
}

void copy_to_list_callback(var_atom_t const* varAtom, void* context){

    pybind11::list& var_atom_list = *( (pybind11::list*)(context) );

    var_atom_list.append(
            std::make_pair(std::string(varAtom->var), CAtom(atom_clone(varAtom->atom))));
}

void atom_copy_to_list_callback(const atom_t* atom, void* context){
    pybind11::list& atoms_list = *( (pybind11::list*)(context) );
    atoms_list.append(CAtom(atom_clone(atom)));
}

void bindings_copy_to_list_callback(bindings_t* bindings, void* context){
    pybind11::list& bindings_list = *( (pybind11::list*)(context) );
    bindings_list.append(CBindings(bindings_clone(bindings)));
}

struct CConstr {

    py::function pyconstr;

    CConstr(py::function pyconstr) : pyconstr(pyconstr) { }

    static void free(void* ptr) {
        CConstr* self = static_cast<CConstr*>(ptr);
        delete self;
    }

    static atom_t* apply(char const* token, void* context) {
        CConstr* self = static_cast<CConstr*>(context);
        py::object atom = self->pyconstr(token);
        return atom_clone(atom.attr("catom").cast<CAtom>().ptr);
    }
};

struct CSExprParser {

    std::string text;
    sexpr_parser_t* ptr;

    CSExprParser(std::string text) : text(text) {
        ptr = sexpr_parser_new(this->text.c_str());
    }

    virtual ~CSExprParser() {
        sexpr_parser_free(ptr);
    }

    py::object parse(CTokenizer tokenizer) {
        atom_t* atom = sexpr_parser_parse(this->ptr, tokenizer.ptr);
        return atom ? py::cast(CAtom(atom)) : py::none();
    }
};

struct CAtomType {};

PYBIND11_MODULE(hyperonpy, m) {
    m.doc() = "Python API of the Hyperon library";

    py::enum_<atom_type_t>(m, "AtomKind")
        .value("SYMBOL", atom_type_t::SYMBOL)
        .value("VARIABLE", atom_type_t::VARIABLE)
        .value("EXPR", atom_type_t::EXPR)
        .value("GROUNDED", atom_type_t::GROUNDED)
        .export_values();

    py::class_<CAtom>(m, "CAtom");

    m.def("atom_sym", [](char const* name) { return CAtom(atom_sym(name)); }, "Create symbol atom");
    m.def("atom_var", [](char const* name) { return CAtom(atom_var(name)); }, "Create variable atom");
    m.def("atom_expr", [](py::list _children) {
            size_t size = py::len(_children);
            atom_t* children[size];
            int idx = 0;
            for (auto& atom : _children) {
                // Copying atom is required because atom_expr() moves children
                // catoms inside new expression atom.
                children[idx++] = atom_clone(atom.cast<CAtom&>().ptr);
            }
            return CAtom(atom_expr(children, size));
        }, "Create expression atom");
    m.def("atom_gnd", [](py::object object, CAtom ctyp) {
            if (py::hasattr(object, "cspace")) {
                //TODO: We should make static constant type atoms, so we don't need to allocate and then
                // free them, just to test a constant 
                atom_t* undefined = ATOM_TYPE_UNDEFINED();
                if (!atom_eq(ctyp.ptr, undefined)) {
                    throw std::runtime_error("Grounded Space Atoms can't have a custom type");
                }
                atom_free(undefined);
                space_t* space = object.attr("cspace").cast<CSpace>().ptr;
                return CAtom(atom_gnd_for_space(space));
            } else {
                atom_t* typ = atom_clone(ctyp.ptr);
                return CAtom(atom_gnd(new GroundedObject(object, typ)));
            }
            }, "Create grounded atom");
    m.def("atom_free", [](CAtom atom) { atom_free(atom.ptr); }, "Free C atom");

    m.def("atom_eq", [](CAtom a, CAtom b) -> bool { return atom_eq(a.ptr, b.ptr); }, "Test if two atoms are equal");
    m.def("atom_to_str", [](CAtom atom) {
            return func_to_string((write_to_buf_func_t)&atom_to_str, atom.ptr);
        }, "Convert atom to human readable string");
    m.def("atom_get_type", [](CAtom atom) { return atom_get_type(atom.ptr); }, "Get type of the atom");
    m.def("atom_get_name", [](CAtom atom) {
            return func_to_string((write_to_buf_func_t)&atom_get_name, atom.ptr);
        }, "Get name of the Symbol or Variable atom");
    m.def("atom_get_space", [](CAtom atom) {
            return CSpace(space_clone_ref(atom_get_space(atom.ptr)));
        }, "Get the space inside of a Grounded atom wrapping a space");
    m.def("atom_get_object", [](CAtom atom) {
            return static_cast<GroundedObject const*>(atom_get_object(atom.ptr))->pyobj;
        }, "Get object of the grounded atom");
    m.def("atom_get_grounded_type", [](CAtom atom) {
            return CAtom(atom_get_grounded_type(atom.ptr));
        }, "Get object of the grounded atom");
    m.def("atom_get_children", [](CAtom atom) {
            py::list atoms;
            atom_get_children(atom.ptr, copy_atoms, &atoms);
            return atoms;
        }, "Get children atoms of the expression");
    m.def("atom_iterate", [](CAtom atom) -> pybind11::list {
            pybind11::list atoms_list;
            atom_iterate(atom.ptr, atom_copy_to_list_callback, &atoms_list);
            return atoms_list;
        }, "Returns iterator to traverse child atoms recursively, depth first");
    m.def("atom_match_atom", [](CAtom a, CAtom b) -> CBindingsSet {
            return CBindingsSet(atom_match_atom(a.ptr, b.ptr));
        }, "Matches one atom against another, establishing Bindings between variables");
    m.def("atoms_are_equivalent", [](CAtom first, CAtom second) {
            return atoms_are_equivalent(first.ptr, second.ptr);
        }, "Check atom for equivalence");

    py::class_<CVecAtom>(m, "CVecAtom");
    m.def("vec_atom_new", []() { return CVecAtom(vec_atom_new()); }, "New vector of atoms");
    m.def("vec_atom_free", [](CVecAtom vec) { vec_atom_free(vec.ptr); }, "Free vector of atoms");
    m.def("vec_atom_size", [](CVecAtom vec) { return vec_atom_size(vec.ptr); }, "Return size of the vector");
    m.def("vec_atom_push", [](CVecAtom vec, CAtom atom) { vec_atom_push(vec.ptr, atom_clone(atom.ptr)); }, "Push atom into vector");
    m.def("vec_atom_pop", [](CVecAtom vec) { return CAtom(vec_atom_pop(vec.ptr)); }, "Push atom into vector");

    py::class_<CBindings>(m, "CBindings");
    m.def("bindings_new", []() { return CBindings(bindings_new()); }, "New bindings");
    m.def("bindings_free", [](CBindings bindings) { bindings_free(bindings.ptr);}, "Free bindings" );
    m.def("bindings_clone", [](CBindings bindings) { return CBindings(bindings_clone(bindings.ptr)); }, "Deep copy of bindings");
    m.def("bindings_merge", [](CBindings left, CBindings right) { return CBindings(bindings_merge(left.ptr, right.ptr));}, "Merges bindings");
    m.def("bindings_merge_v2", [](CBindings self, CBindings other) {
        return CBindingsSet(bindings_merge_v2(bindings_clone(self.ptr), other.ptr));
    }, "Merges bindings into a BindingsSet, allowing for conflicting bindings to split");
    m.def("bindings_eq", [](CBindings left, CBindings right){ return bindings_eq(left.ptr, right.ptr);}, "Compares bindings"  );
    m.def("bindings_add_var_bindings",
          [](CBindings bindings, char const* varName, CAtom atom) {
              var_atom_t var_atom{.var = varName, .atom = atom_clone(atom.ptr) };
              return bindings_add_var_binding(bindings.ptr, &var_atom);
          },
          "Links variable to atom" );
    m.def("bindings_is_empty", [](CBindings bindings){ return bindings_is_empty(bindings.ptr);}, "Returns true if bindings is empty");

    m.def("bindings_narrow_vars", [](CBindings bindings, CVecAtom vars) {
            bindings_narrow_vars(bindings.ptr, vars.ptr);
        }, "Remove vars from Bindings, except those specified" );

    m.def("bindings_resolve", [](CBindings bindings, char const* varName) -> std::optional<CAtom> {
            auto const res = bindings_resolve(bindings.ptr, varName);
            return nullptr == res ? std::nullopt : std::optional(CAtom(res));
        }, "Resolve" );

    m.def("bindings_resolve_and_remove", [](CBindings bindings, char const* varName) -> std::optional<CAtom> {
            auto const res = bindings_resolve_and_remove(bindings.ptr, varName);
            return nullptr == res ? std::nullopt : std::optional(CAtom(res));
        }, "Resolve and remove" );

    m.def("bindings_to_str", [](CBindings bindings) {
        return func_to_string((write_to_buf_func_t)&bindings_to_str, bindings.ptr);
    }, "Convert bindings to human readable string");

    m.def("bindings_list", [](CBindings bindings) -> pybind11::list {
        pybind11::list var_atom_list;
        bindings_traverse(
                bindings.ptr,
                copy_to_list_callback,
                &var_atom_list);

        return var_atom_list;
    }, "Returns iterator to traverse bindings");

    py::class_<CBindingsSet>(m, "CBindingsSet");
    m.def("bindings_set_empty", []() { return CBindingsSet(bindings_set_empty()); }, "New BindingsSet with no Bindings");
    m.def("bindings_set_single", []() { return CBindingsSet(bindings_set_single()); }, "New BindingsSet with one new Bindings");
    m.def("bindings_set_free", [](CBindingsSet set) { bindings_set_free(set.ptr); }, "Free BindingsSet");
    m.def("bindings_set_eq", [](CBindingsSet set, CBindingsSet other) { return bindings_set_eq(set.ptr, other.ptr); }, "Free BindingsSet");
    //TODO: I think we need better words for these concepts.  "empty" & "single" are placeholders for now.
    //https://github.com/trueagi-io/hyperon-experimental/issues/281
    m.def("bindings_set_is_empty", [](CBindingsSet set) {
        return bindings_set_is_empty(set.ptr);
    }, "Returns true if BindingsSet contains no Bindings object (fully constrained)");
    m.def("bindings_set_is_single", [](CBindingsSet set) {
        return bindings_set_is_single(set.ptr);
    }, "Returns true if BindingsSet contains no variable bindings (unconstrained)");
    m.def("bindings_set_to_str", [](CBindingsSet set) {
        return func_to_string((write_to_buf_func_t)&bindings_set_to_str, set.ptr);
    }, "Convert BindingsSet to human readable string");
    m.def("bindings_set_clone", [](CBindingsSet set) { return CBindingsSet(bindings_set_clone(set.ptr)); }, "Deep copy of BindingsSet");
    m.def("bindings_set_from_bindings", [](CBindings bindings) { bindings_t* cloned_bindings = bindings_clone(bindings.ptr); return CBindingsSet(bindings_set_from_bindings(cloned_bindings)); }, "New BindingsSet from existing Bindings");
    m.def("bindings_set_push", [](CBindingsSet set, CBindings bindings) { bindings_t* cloned_bindings = bindings_clone(bindings.ptr); bindings_set_push(set.ptr, cloned_bindings); }, "Adds the Bindings to the BindingsSet");
    m.def("bindings_set_add_var_binding", [](CBindingsSet set, CAtom var, CAtom value) {
        bindings_set_add_var_binding(set.ptr, var.ptr, value.ptr);
    }, "Asserts a binding between a variable and an atom for every Bindings in the BindingsSet" );
    m.def("bindings_set_add_var_equality", [](CBindingsSet set, CAtom var_a, CAtom var_b) {
        bindings_set_add_var_equality(set.ptr, var_a.ptr, var_b.ptr);
    }, "Asserts a binding between two variables for every Bindings in the BindingsSet" );
    m.def("bindings_set_merge_into", [](CBindingsSet set, CBindingsSet other) {
        bindings_set_merge_into(set.ptr, other.ptr);
    }, "Merges the contents of the `other` BindingsSet into the `set` BindingsSet" );
    m.def("bindings_set_list", [](CBindingsSet set) -> pybind11::list {
        pybind11::list bindings_list;
        bindings_set_iterate(
                set.ptr,
                bindings_copy_to_list_callback,
                &bindings_list);

        return bindings_list;
    }, "Returns iterator to traverse Bindings within BindingsSet");

    py::class_<CSpace>(m, "CSpace");
    m.def("space_new_grounding", []() { return CSpace(space_new_grounding_space()); }, "New grounding space instance");
    m.def("space_new_custom", [](py::object object) {
        return CSpace( space_new(&PY_SPACE_NO_SUBST_API, new PySpace(object)));
        }, "Create new custom space implemented in Python");
    m.def("space_free", [](CSpace space) { space_free(space.ptr); }, "Free space");
    m.def("space_get_payload", [](CSpace space) {
        PySpace* py_space = (PySpace*)space_get_payload(space.ptr);
        return py_space->pyobj;
        }, "Accessor for the payload of a space implemented in Python");
    m.def("space_add", [](CSpace space, CAtom atom) { space_add(space.ptr, atom_clone(atom.ptr)); }, "Add atom into space");
    m.def("space_remove", [](CSpace space, CAtom atom) { return space_remove(space.ptr, atom.ptr); }, "Remove atom from space");
    m.def("space_replace", [](CSpace space, CAtom from, CAtom to) { return space_replace(space.ptr, from.ptr, atom_clone(to.ptr)); }, "Replace atom from space");
    m.def("space_eq", [](CSpace a, CSpace b) { return space_eq(a.ptr, b.ptr); }, "Check if two spaces are equal");
    m.def("space_atom_count", [](CSpace space) { return space_atom_count(space.ptr); }, "Return number of atoms in space, or -1 if the space is unable to determine the value");
    m.def("space_list", [](CSpace space) -> std::optional<pybind11::list> {
        pybind11::list atoms_list;
        if (space_iterate(space.ptr, atom_copy_to_list_callback, &atoms_list)) {
            return atoms_list;
        } else {
            return std::nullopt;
        }
    }, "Returns iterator to traverse atoms within a space");
    m.def("space_query", [](CSpace space, CAtom pattern) {
            py::list results;
            space_query(space.ptr, pattern.ptr,
                    [](bindings_t const* cbindings, void* context) {
                        py::list& results = *(py::list*)context;
                        py::dict pybindings;
                        bindings_traverse(cbindings, copy_atom_to_dict, &pybindings );
                        results.append(pybindings);
                    }, &results);
            return results;
        }, "Query atoms from space by pattern");
    m.def("space_subst", [](CSpace space, CAtom pattern, CAtom templ) {
            py::list atoms;
            space_subst(space.ptr, pattern.ptr, templ.ptr, copy_atoms, &atoms);
            return atoms;
        }, "Get bindings for pattern and apply to template");

    py::class_<CTokenizer>(m, "CTokenizer");
    m.def("tokenizer_new", []() { return CTokenizer(tokenizer_new()); }, "New tokenizer");
    m.def("tokenizer_free", [](CTokenizer tokenizer) { tokenizer_free(tokenizer.ptr); }, "Free tokenizer");
    m.def("tokenizer_clone", [](CTokenizer tokenizer) { tokenizer_clone(tokenizer.ptr); }, "Clone tokenizer");
    m.def("tokenizer_register_token", [](CTokenizer tokenizer, char const* regex, py::function constr) {
            droppable_t context = { new CConstr(constr), CConstr::free };
            tokenizer_register_token(tokenizer.ptr, regex, &CConstr::apply, context);
        }, "Register token");

    py::class_<CSExprParser>(m, "CSExprParser")
        .def(py::init<std::string>())
        .def("parse", &CSExprParser::parse,  "Return next parser atom or None");

    py::class_<CStepResult>(m, "CStepResult")
        .def("__str__", [](CStepResult step) {
            return func_to_string((write_to_buf_func_t)&step_to_str, step.ptr);
        }, "Convert step to human readable string");
    m.def("interpret_init", [](CSpace space, CAtom expr) {
            return CStepResult(interpret_init(space.ptr, expr.ptr));
        }, "Initialize interpreter of the expression");
    m.def("interpret_step", [](CStepResult step) {
            return CStepResult(interpret_step(step.ptr));
        }, "Do next step of the interpretataion");
    m.def("step_has_next", [](CStepResult step) {
            return step_has_next(step.ptr);
        }, "Check whether next step of interpretation is posible");
    m.def("step_get_result", [](CStepResult step) {
            py::list atoms;
            step_get_result(step.ptr, copy_atoms, &atoms);
            return atoms;
        }, "Return result of the interpretation");

#define ADD_TYPE(t, d) .def_property_readonly_static(#t, [](py::object) { return CAtom(ATOM_TYPE_ ## t()); }, d " atom type")

    py::class_<CAtomType>(m, "CAtomType")
        ADD_TYPE(UNDEFINED, "Undefined")
        ADD_TYPE(TYPE, "Type")
        ADD_TYPE(ATOM, "Generic")
        ADD_TYPE(SYMBOL, "Symbol")
        ADD_TYPE(VARIABLE, "Variable")
        ADD_TYPE(EXPRESSION, "Expression")
        ADD_TYPE(GROUNDED, "Grounded")
        ADD_TYPE(GROUNDED_SPACE, "Space");
    m.def("check_type", [](CSpace space, CAtom atom, CAtom type) {
            return check_type(space.ptr, atom.ptr, type.ptr);
        }, "Check if atom is an instance of the passed type");
    m.def("validate_atom", [](CSpace space, CAtom atom) {
            return validate_atom(space.ptr, atom.ptr);
        }, "Validate expression arguments correspond to the operation type");
    m.def("get_atom_types", [](CSpace space, CAtom atom) {
            py::list atoms;
            get_atom_types(space.ptr, atom.ptr, copy_atoms, &atoms);
            return atoms;
        }, "Get types of the given atom");

    py::class_<CMetta>(m, "CMetta");
    m.def("metta_new", [](CSpace space, CTokenizer tokenizer, char const* cwd) {
        return CMetta(metta_new(space.ptr, tokenizer.ptr, cwd));
    }, "New MeTTa interpreter instance");
    m.def("metta_free", [](CMetta metta) { metta_free(metta.ptr); }, "Free MeTTa interpreter");
    m.def("metta_clone", [](CMetta metta) { metta_clone(metta.ptr); }, "Clone MeTTa interpreter");
    m.def("metta_space", [](CMetta metta) { return CSpace(metta_space(metta.ptr)); }, "Get space of MeTTa interpreter");
    m.def("metta_tokenizer", [](CMetta metta) { return CTokenizer(metta_tokenizer(metta.ptr)); }, "Get tokenizer of MeTTa interpreter");
    m.def("metta_run", [](CMetta metta, CSExprParser& parser) {
            py::list lists_of_atom;
            metta_run(metta.ptr, parser.ptr, copy_lists_of_atom, &lists_of_atom);
            return lists_of_atom;
        }, "Run MeTTa interpreter on an input");
    m.def("metta_evaluate_atom", [](CMetta metta, CAtom atom) {
            py::list atoms;
            metta_evaluate_atom(metta.ptr, atom_clone(atom.ptr), copy_atoms, &atoms);
            return atoms;
        }, "Run MeTTa interpreter on an atom");

    m.def("metta_load_module", [](CMetta metta, std::string text) {
        metta_load_module(metta.ptr, text.c_str());
    }, "Load MeTTa module");

}

__attribute__((constructor))
static void init_library() {
    // TODO: integrate Rust logs with Python logger
    init_logger();
}

