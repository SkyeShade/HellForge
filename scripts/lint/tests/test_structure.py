"""structure.py: tokens, functions and metrics of C#, C and C++."""
import sys
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import structure  # noqa: E402


def model(code: str, language: str) -> structure.Model:
    return structure.analyse(textwrap.dedent(code), language)


def functions(code: str, language: str) -> dict:
    return {f.name: f for f in model(code, language).functions}


def one(code: str, language: str) -> structure.Function:
    found = model(code, language).functions
    assert len(found) == 1, [f.name for f in found]
    return found[0]


class Tokens(unittest.TestCase):
    def values(self, code: str, language: str) -> list[str]:
        return [t.value for t in structure.tokenize(textwrap.dedent(code), language)[0]]

    def test_comments_and_directives(self):
        code = '''
        #define A 1 /* a comment that
                       goes on */
        #define B(x) \\
            ((x) + 1)
        int f(void) { return A; } // done
        '''
        self.assertEqual(self.values(code, 'c'), ['int', 'f', '(', 'void', ')', '{', 'return', 'A', ';', '}'])

    def test_if_branches(self):
        code = '''
        #ifdef WIN
        if (a) {
        #else
        if (b) {
        #endif
        }
        #if 0
        x
        #else
        y
        #endif
        '''
        self.assertEqual(self.values(code, 'c'), ['if', '(', 'a', ')', '{', '}', 'y'])
        with self.assertRaises(structure.ParseError):
            structure.tokenize('#if X\nint a;\n', 'c')

    def test_c_literals(self):
        values = self.values(r'''x = L"a\"b"; c = 'q'; d = '\''; r = R"tag(a ")" b)tag"; n = 1'000'000; u = u8"z";''',
                             'cpp')
        self.assertIn('R""', values)
        self.assertIn("1'000'000", values)
        self.assertIn("'\\''", values)
        self.assertEqual(values.count(';'), 6)

    def test_cs_strings(self):
        code = r'''
        var a = $"x {b} {{literal}} {(c ? "y" : $"z{d}")} \" end";
        var v = @"C:\path""quoted";
        var r = """
            raw " text } {
            """;
        var i = $@"{e}\n";
        char q = '"';
        '''
        values = self.values(code, 'cs')
        self.assertEqual(values.count(';'), 5)
        self.assertEqual(values.count('var'), 4)
        self.assertNotIn('}', values)

    def test_unbalanced(self):
        with self.assertRaises(structure.ParseError):
            structure.analyse('void f() { if (a) { }', 'c')


class Functions(unittest.TestCase):
    def test_c(self):
        found = functions('''
        static int add(int a, int b) { return a + b; }
        int proto(int a);
        static struct point *make(void) { return 0; }
        __declspec(dllexport) int __cdecl init(const HfApi *api, void *mod) { return 0; }
        static const struct ops table = { .run = add, .size = sizeof(int) };
        int (*pointer)(int);
        void none(void) { }
        void dots(const char *f, ...) { }
        ''', 'c')
        self.assertEqual(sorted(found), ['add', 'dots', 'init', 'make', 'none'])
        self.assertEqual(found['add'].parameters, 2)
        self.assertEqual(found['none'].parameters, 0)
        self.assertEqual(found['dots'].parameters, 1)

    def test_cpp(self):
        code = '''
        namespace ui {
        struct Panel {
            int Width() const { return w; }
            Panel(int a) : w(a), h{2} { }
        };
        }
        extern "C" {
        int Exported(void) { return 1; }
        }
        void ui::Panel::Draw(int x) noexcept {
            auto f = [&](int y) { return y + x; };
            std::sort(a, b, [](int l, int r) { return l < r; });
        }
        bool operator==(const A &a, const A &b) { return a.v == b.v; }
        '''
        found = functions(code, 'cpp')
        self.assertIn('Panel::Width', found)
        self.assertIn('Panel::Panel', found)
        self.assertIn('Exported', found)
        self.assertIn('ui::Panel::Draw', found)
        self.assertEqual(sum(1 for f in model(code, 'cpp').functions if f.name.endswith('<lambda>')), 2)
        self.assertIn('operator==', found)
        self.assertEqual(found['ui::Panel::Draw'].cognitive, 0)

    def test_cs(self):
        found = functions('''
        namespace N;
        public sealed record Pair(int A, int B) { public int Sum() => A + B; }
        public class C : Base, IThing where T : class
        {
            [Attr(1)] public C(int x) : base(x) { }
            public int Size { get; set; } = 4;
            public int Twice => Size * 2;
            public string this[int i] { get { return ""; } set { } }
            event Action E { add { } remove { } }
            readonly Func<int, int> f = x => { return x; };
            public T Get<T>() where T : new() => new T();
            public static C operator +(C a, C b) => a;
            void Work()
            {
                var o = new Thing(1) { A = 2 };
                int Local(int v) { return v; }
                static int Short(int v) => v * 2;
                list.ForEach(item => { Use(item); });
                Run(delegate (int z) { });
            }
        }
        ''', 'cs')
        names = sorted(found)
        for expected in ('Pair.Sum', 'C.C', 'C.Twice', 'C.this.get', 'C.this.set', 'C.E.add', 'C.E.remove',
                         'C.Get', 'C.operator+', 'C.Work', 'C.Work.Local', 'C.Work.Short', 'C.Work.<lambda>',
                         'C.Work.<delegate>', '<lambda>'):
            self.assertIn(expected, names)
        self.assertNotIn('Pair', names)                     # a primary constructor is not a function
        self.assertEqual(found['C.C'].parameters, 1)
        self.assertEqual(found['C.operator+'].parameters, 2)


class Complexity(unittest.TestCase):
    def test_sonar_sum_of_primes(self):
        function = one('''
        int sumOfPrimes(int max) {
            int total = 0;
            for (int i = 1; i <= max; ++i) {
                for (int j = 2; j < i; ++j) {
                    if (i % j == 0) {
                        goto next;
                    }
                }
                total += i;
            next:;
            }
            return total;
        }
        ''', 'c')
        self.assertEqual(function.cognitive, 7)
        self.assertEqual(function.nesting, 3)

    def test_sonar_switch(self):
        function = one('''
        string GetWords(int number) {
            switch (number) {
                case 1: return "one";
                case 2: return "a couple";
                default: return "lots";
            }
        }
        ''', 'cs')
        self.assertEqual(function.cognitive, 1)

    def test_else_and_boolean_runs(self):
        function = one('''
        void F(bool a, bool b, bool c) {
            if (a && b && c) { }          // +1 if, +1 run
            else if (a || b && c) { }     // +1 else if, +2 runs
            else { if (x) { } }           // +1 else, +2 nested if
            bool d = a && (b || f(c && a)) && b;   // +1 &&, +1 ||, +1 && in the call
        }
        ''', 'cs')
        self.assertEqual(function.cognitive, 1 + 1 + 1 + 2 + 1 + 2 + 3)

    def test_ternary_loops_catch_and_recursion(self):
        function = one('''
        int R(int n) {
            try {
                while (n > 0) {           // +1
                    n = n > 5 ? R(n - 1) : n;   // +2 (nesting 1), +1 recursion
                }
            } catch (Exception e) when (e.Data != null && n > 0) {   // +1, +1 run
                do { n--; } while (n > 0);  // +2 (nesting 1)
            } finally { }
            return n switch { 0 => 1, _ => 2 };   // +1
        }
        ''', 'cs')
        self.assertEqual(function.cognitive, 1 + 2 + 1 + 1 + 1 + 2 + 1)

    def test_nullable_is_not_a_ternary(self):
        function = one('''
        void F() {
            int? a = null;
            string? s = b ? "x" : "y";
            var list = new List<int?>();
            var n = (int?)c;
        }
        ''', 'cs')
        self.assertEqual(function.cognitive, 1)

    def test_lambdas_are_measured_alone(self):
        found = functions('''
        void Outer() {
            if (a) { }
            Run(() => { if (b) { if (c) { } } });
            var e = items.Where(x => x.A && x.B);
        }
        ''', 'cs')
        self.assertEqual(found['Outer'].cognitive, 1 + 1)
        self.assertEqual(found['Outer.<lambda>'].cognitive, 1 + 2)

    def test_macro_block_and_labels(self):
        function = one('''
        void f(void) {
            FOR_EACH(item, list) {
                if (item) { }
            }
        retry:
            switch (x) { case A: break; default: goto retry; }
        }
        ''', 'c')
        self.assertEqual(function.cognitive, 1 + 1 + 1)


class Measures(unittest.TestCase):
    def test_lines(self):
        found = functions('''
        void Outer() {
            int a = 1;

            // a comment line
            Run(() => {
                Use(a);
            });
        }
        ''', 'cs')
        self.assertEqual(len(found['Outer'].lines), 5)            # the lambda's first line counts for both
        self.assertEqual(len(found['Outer.<lambda>'].lines), 3)

    def test_stack(self):
        function = one('''
        void f(void) {
            char path[MAX_PATH];
            wchar_t wide[2 * 1024], other[8];
            char *names[16];
            static char kept[1 << 20];
            struct item items[100];
            uint8_t data[COUNT];
        }
        ''', 'c')
        self.assertEqual(function.stack_bytes, 260 + 4096 + 16 + 128)

    def test_array_sizes(self):
        """Sizes are C arithmetic, without an exponent and bounded: `9**9**9` is no size, and is refused at once."""
        sizes = {'2 * 1024': 2048, '1 << 20': 1 << 20, '(2 + 3) * 4 << 1': 40, '16 / 3': 5, 'MAX_PATH * 2': 520,
                 '0x10 + 1': 17, '9**9**9': None, '9 ** 9': None, '1 << 100': None, '-4': None, '4 / 0': None,
                 '1 << 63 << 1': None}
        for size, want in sizes.items():
            function = one(f'void f(void) {{ char a[{size}]; }}', 'c')
            self.assertEqual(function.stack_bytes or None, want, size)

    def test_module_lines(self):
        self.assertEqual(model('#include <a.h>\n\nint a;\n// note\nint b;\n', 'c').code_lines, 3)

    def test_duplicates(self):
        body = ' '.join(f'x{i} = {i};' for i in range(30))
        first = structure.analyse(f'void a() {{ {body} }}', 'c').tokens
        second = structure.analyse(f'void b() {{ {body} }}', 'c').tokens
        clones = structure.duplicates([first, second], 100)
        self.assertEqual(len(clones), 2)
        self.assertTrue(all(c.end - c.start >= 100 for c in clones))
        self.assertEqual(structure.duplicates([first], 100), [])


if __name__ == '__main__':
    unittest.main()
