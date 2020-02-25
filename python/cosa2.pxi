from cython.operator cimport dereference as dref
from cosa2 cimport TransitionSystem as c_TransitionSystem

from smt_switch cimport SmtSolver, Sort, Term


cdef class TransitionSystem:
    cdef c_TransitionSystem* cts
    cdef SmtSolver _solver
    def __cinit__(self, SmtSolver s):
        cts = new c_TransitionSystem(s.css)
        self._solver = s

    def set_behavior(self, Term init, Term trans):
        dref(self.cts).set_behavior(init.ct, trans.ct)

    def set_init(self, Term init):
        dref(self.cts).set_init(init.ct)

    def constrain_init(self, Term constraint):
        dref(self.cts).constrain_init(constraint.ct)

    def set_trans(self, Term trans):
        dref(self.cts).set_trans(trans.ct)

    def constrain_trans(self, Term constraint):
        dref(self.cts).constrain_trans(constraint.ct)

    def assign_next(self, Term state, Term val):
        dref(self.cts).assign_next(state.ct, val.ct)

    def add_invar(self, Term constraint):
        dref(self.cts).add_invar(constraint.ct)

    def constrain_inputs(self, Term constraint):
        dref(self.cts).constrain_inputs(constraint.ct)

    def add_constraint(self, Term constraint):
        dref(self.cts).add_constraint(constraint.ct)

    def name_term(self, str name, Term t):
        dref(self.cts).name_term(name.encode(), t.ct)

    def make_input(self, str name, Sort sort):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).make_input(name.encode(), sort.cs)
        return term

    def make_state(self, str name, Sort sort):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).make_state(name.encode(), sort.cs)
        return term

    def curr(self, Term t):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).curr(t.ct)
        return term

    def next(self, Term t):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).next(t.ct)
        return term

    def is_curr_var(self, Term sv):
        return dref(self.cts).is_curr_var(sv.ct)

    def is_next_var(self, Term sv):
        return dref(self.cts).is_next_var(sv.ct)

    @property
    def solver(self):
        return self._solver

    # TODO: uncomment these (might need more iteration operators)
    # @property
    # def states(self):
    #     states_set = set()

    #     cdef Term term = Term(self._solver)
    #     for s in dref(self.cts).states():
    #         term.ct = s
    #         states_set.insert(term)

    #     return states_set

    # @property
    # def inputs(self):
    #     inputs_set = set()

    #     cdef Term term = Term(self._solver)
    #     for s in dref(self.cts).inputs():
    #         term.ct = s
    #         inputs_set.insert(term)

    #     return inputs_set

    # @property
    # def state_updates(self):
    #     updates = {}
    #     cdef Term k = Term(self._solver)
    #     cdef Term v = Term(self._solver)

    #     for elem in dref(self.cts).state_updates():
    #         k.ct = elem.first
    #         v.ct = elem.second
    #         updates[k] = v

    #     return updates

    # @property
    # def named_terms(self):
    #     names2terms = {}

    #     cdef Term term = Term(self._solver)
    #     for elem in dref(self.cts).named_terms():
    #         term.ct = elem.second
    #         names2terms[elem.first.decode()] = term

    #     return names2terms

    @property
    def init(self):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).init()
        return term

    @property
    def trans(self):
        cdef Term term = Term(self._solver)
        term.ct = dref(self.cts).trans()
        return term

    def is_functional(self):
        return dref(self.cts).is_functional()