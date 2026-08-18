# Classes

FFlags:

- LuauBetterUserDefinedClasses
- DebugLuauUserDefinedClasses
- DebugLuauUserDefinedClassesRuntime

## Summary

Add user-defined classes with new primitives `class` (the class definition) and `object` (instances of a class).

```luau
class Cat
    name: string
    age: number
end
const taz = Cat { name = "Taz", age = 14 }

export class User
    public id: string
    public name: string
    public dob: DateTime

    private function __init(self, name, dob)
        self.id = generate_id()
        self.name = name
        self.dob = dob
    end

    public function new(name: string, dob: DateTime): User
        const self = User(name, dob)
        return self
    end

    public function is_builders_club(self): boolean
        -- ...
    end
end
```

## Motivation

Classes are an incredibly common way to encapsulate data structures and behavior in many programming languages, but our language doesn't support it officially and requires users to spend more time (and space) writing boilerplate (`__index`, `setmetatable`, `export type Classy = typeof(Classy.constructor(...)))`) to emulate the concept. We should make it easier and more performant.

As of today, the upstream Luau team has partly implemented classes, but its implementation is not finished and has both runtime and static analysis bugs we need to fix. Completeness aside, the Luau team's recent decisions around class-related RFCs feel strange and seem locked to a particular design without considering other possibilities for the feature. We would prefer a different design with semantics that would be more beneficial towards the community.

Nonetheless, we should mention some of their original "Classes!" RFC's motivations, since they hold equally true today:

> - People write object-oriented code. We should afford it in a polished way.
> - Accurate type inference of `setmetatable` has proven to be very difficult to get right. Because of this, the quality of our autocomplete isn't what it could be.
> - A construct with a fixed shape and a completely locked-down metatable will open up optimization opportunities that could improve performance:
>   - If a value is known to be an instance of a particular class, the bytecode compiler should be able optimize method calls to skip the whole `__index` metamethod process and instead generate code to directly call the correct method.
>   - By the same token, method calls can be inlined more aggressively.  Particularly self-method calls eg `self:SomeOtherMethod()`
>   - Field accesses can compile to a simple integral table offset so that the VM doesn't need to do a hashtable lookup as the program runs.
>   - Since every instance of a class has the same set of properties, we can split the hash table: The set of fields can be associated with the class and instances only need to carry the values of those fields.  We think this can improve performance by improving cache locality.
> - Encapsulation at its current state cannot be truly achieved, tables cannot truly be locked-down, and most workarounds for it are too complex for what it's trying to achieve.

In this RFC, we'll be focusing on a base design for classes that allows us to later implement one or both of the following: interfaces and/or inheritance.

## Design

Firstly, we introduce the new `class` and `object` primitives that represent the class definition (and object factory) as well as actual objects (instances) of the class. We chose a new primitive that has an exact field structure with locked down field shape and metatable for future optimization potential.

Our design of classes aims to equally support 2 primary usecases, "POD" and "heavy".

The POD (plain old data) usecase supports users who want to use classes as "named tables" (or structs) to describe mostly data with maybe a few functions. We don't want to increase verbosity and force access specifiers, etc. when using classes as lightweight data structures.

The other main usecase is "heavy", when the user opts into encapsulation (`private`), access specifiers, const members, and in the future, composition, interfaces, and inheritance.

### Class definition syntax

Classes are created with a new class definition syntax:

```luau
class ClassName end

class Cat
    name: string
    age: number
    -- all fields are public, 'public' keyword not required on fields/methods
    function meow(self)
        print(`{self.name} says meow!`)
    end
end

class List<T>
    private inner: { T }
    private function __init(self, initial: { T }?, capacity: number)
        -- ...
    end
    public function with_capacity(cap: number): List<T> -- class has private members, 'public' keyword required
        return List(nil, cap)
    end
end
```

Specifically:

- A class definition starts with the contextual keyword `class`,
- is followed by the class's name and an optional generics parameter list (`<A, B, ...>`),
- contains zero or more class member (fields and methods) declarations (see below)
- ends with the `end` keyword.

Class definitions are a block construct like `for` loops, and do not evaluate to a value.

Instead, evaluating a class definition scopes the class to the entire module, similarly to a global.
Due to this, defining two classes with the same name in the same module is currently forbidden.

Classes must be defined in the top level of a module; attempting to define a class anywhere else raises a syntax error.
This is a limitation imposed by upstream Luau, and we hope to loosen this restriction later.

### Class member syntax

We introduce two specific flavors of keywords to help introduce class members: access specifiers and modifiers.

- Access specifiers: `public`, `private`
- Modifiers: `const`

- Fields are introduced with the new access specifier keywords (or a bare identifier if all fields are public).
- Fields on a class are mutable by default but functions/methods on a class are `const` by default.
- For now, we plan to only implement `public` and `private`. We may look into implementing other access specifiers such as `protected` in the future.
- Fields may include the `const` modifier.

We reuse the `const` keyword to mean "this field is set at class definition evaluation time and may not be modified".

A previous version of this RFC introduced a `static` modifier. We choose to not introduce a static modifier because both static fields and methods can be handled with existing syntax. Static functions are just functions on classes without a `self` parameter. Static fields can be emulated via module-level upvalue (`const`, `local`, and/or exported). We believe the advantage of having a `private` `static` variable (the only functionality not addressed by not adding `static`) is not substantial enough motivation to add a whole keyword to the language; users can just make a module to hold their class with a `local` or `const` static value if they absolutely don't want code unrelated to that class to touch it. Additionally, `static` is a weird (nonobvious) word and would lead to even further keyword soup than we are already introducing with this RFC.

```luau
-- user.luau
--- I am basically a static field!!
local last_id = 0

export class User
    public id: string

    private function __init(self)
        last_id += 1
        self.id = "ID " .. tostring(last_id)
    end
end
```

Methods are introduced with the familiar `function` keyword and follow existing `function` definition syntax.

- All functions on a class via familiar `function` syntax are `const` and may not be mutated.
- A function that doesn't take `self` as its first parameter is a static function.
- A function that takes `self` as its first parameter is a method. The `self` parameter name is hardcoded, like in Rust.

Specifically:

- If a class only has public members, the `public` keyword may be omitted,
- If a class has members with any other access specifier other than `public`, then the access specifier is required,
- A modifier `const` may optionally follow the access specifier. `const` must follow the access specifier.
- If the member is a field, a valid identifier with an optional type annotation should follow,
- If the member is a function, use the familiar `function` definition syntax.

Since all functions on classes are inherently const, explicitly defining a `const function` inside a class should also raise a syntax error. We raise a syntax error for this because `const function` syntax would otherwise be valid both inside and outside a class, and such a function could easily be unintentionally moved or copy/pasted inside a class block instead of the module's top level scope.

### Access Specifiers

Access specifiers allows the user to control access to a specific field within a class. For the scope of this RFC, we will only be introducing the `public` and `private` access specifiers, although we plan on investigating `protected` as well as granular visibility (like `pub(crate)` in Rust) at a future date.

#### `public` access specifier

We introduce the `public` keyword to define fields as public, and accessible from everywhere.
This is a contextual keyword that only applies within class member declarations.

Due to existing prior art in Luau of everything being public (tables), and to facilitate POD (plain old data) forms of classes, we felt it makes most sense to treat all fields on a class as `public` by default.

This means, if all fields on a class are public, the user can omit the `public` keyword in front of the field definitions:

```luau
class Vector3
   x: number -- public keyword can be omitted here, all fields are public.
   y: number
   z: number
end
```

To reduce ambiguity, if a class defines a field with any access specifier other than `public`, then the class must specify access specifiers for **all** members:

```luau
class Vector4
    x: number -- Syntax error: class contains non-public members; add `public` keyword to all public fields to prevent ambiguity
    y: number
    z: number
    private w: number
end
```

We acknowledge that omitting type annotations here can look pretty bad, but we feel that the ability to use classes without worrying about access specifiers outweighs the minority of people who would use this brand new feature with zero type annotations.

An example of a badly formatted (and unannotated) but valid class definition:

```luau
class Employee id
    name
    age location
end
```

#### `private` access specifier

To achieve full encapsulation, we introduce the `private` access specifier.
This is a contextual keyword that only applies within class member declarations.

If a member is marked as private, it is only accessible from within its enclosing class definition block,
and is therefore locked to the outside world.

Functions within classes may only access `private` fields on their own class, and never any private field on any other class.

Attempting to access a `private` member from outside its class definition block results in a runtime error.
This includes functions outside the class that were called from a function within its class definition block.

```luau
class User
    public first_name: string
    public last_name: string
    private ssn: string?

    public function name(self): string
        return self.first_name .. " " .. self.last_name
    end

    private function get_ssn(self): string
        if self.ssn then
            return self.ssn
        end
        return get_ssn_from_files(self)
    end
end

const user = User { -- The default constructor can initialize private fields.
    first_name = "Taz",
    last_name = "Parekh",
    ssn = "126-222-1123",
}
```

If a class only has `private` fields, we raise a type error because such a class will not be usable.

```luau
class UseMe -- TypeError: this class cannot be used because it only has private members
    private please: string
    private uses: number
end
```

### `const` modifier

The `const` modifier may only be applied to fields (all functions/methods are always `const`), and should be placed after an access specifier.

A `const` field must be initialized with a value, by the class's constructor. As noted below, `const` fields are not enforced as being const during class construction, to allow the class constructor to modify the fields explictly, pass them to functions that do, etc.

### Metamethods

Classes may define only one `__init` constructor, that may be `public` or `private`.

- `__init`

The following metamethods apply to instances of the class (`object`s), not the class itself.

They all work just like they do on a metatable:

- `__call`
- `__concat`
- `__unm`
- `__add`
- `__sub`
- `__mul`
- `__div`
- `__mod`
- `__pow`
- `__tostring`
- `__eq`
- `__lt`
- `__le`
- `__iter`
- `__len`
- `__idiv`

\* `__init` is not a metamethod per se but we call it out here as a valid method to define on a class.

For now, `__index` and `__newindex` are forbidden in classes. We will most likely re-visit this later.
For forward-compatibility, it is a syntax error to define any other method whose name starts with two underscores.

Keep in mind that only `__init` applies to the class and the object; defining any of the other metamethods
defines them for **`objects`** (instances) of the class instead of the class itself.
It is impossible to define custom metamethods for a `class`, only `object`s of a class.

### The `class` primitive

The action of evaluating a class definition statement introduces a *class* value in the module scope.

A `class` is a value that serves as a factory for instances of the class and as a namespace for any functions that are defined on the class.

Classes are always `const` and frozen.

Taking references to class methods via `ClassName.method` syntax is allowed so that classes can easily compose with existing APIs:

```luau
local n = pcall(SomeClass.getName, someClassObject)
```

The top type of all classes is named `class`.  `type()` and `typeof()` return `"class"` when passed a class.

### The `object` primitive

Objects, often referred to as "class instances", are a new type of value in the VM. They are similar but not quite the same as tables. They have no array part, for instance.

`pairs`, `ipairs` , `getmetatable`, and `setmetatable` all raise a runtime error when invoked on an object. Similarly, an object may not be iterated over unless its class implements `__iter`.

Reading or writing a nonexistent class property raises an error. This makes it easy to disambiguate between a nonexistent property and a property whose value is nil.

We introduce a new top type for instances of a class: `object`. The builtin `type()` and `typeof()` functions return `"object"` for any class instance.

We chose this over having them return the class name because class names do not have to be globally unique (they must only unique within a single module) and because we do not want to make it possible for classes to impersonate embedder-provided types.

```luau
class Cls end
local inst = Cls()

type(Cls) == "class"
typeof(Cls) == "class"

type(inst) == "object"
typeof(inst) == "object"
```

Comparisons between object instances are the same as with tables: If `__eq` is not defined, object comparisons use physical (pointer) equality.  `__eq` is only invoked if both operands are the same type.

### The `class` library

We introduce a new global library `class`. Its contents are:

```luau
local class: {
    isinstance: (o: unknown, C: class) -> boolean,
    classof: (o: unknown) -> class?,
}
```

This library also serves as an obvious extension point for future features like reflection. In the future, we may allow classes to opt-out of reflection using this library.

The function `class.isinstance(o, Class)` returns `true` if the object `o` is an instance of `Class`. At runtime, it raises an error if the second argument is not a class. If the first argument is not an `object`, `class.isinstance` returns false. (eg `class.isinstance(5, MyClass)`)

The `class.classof` function returns the class corresponding to the first argument. If the first argument is not an `object`, the result is `nil`.

### Constructors

#### Rationale

Right now in Luau 0.730 this is valid syntax (with the classes feature enabled)

```luau
class Cat
    public name: string
    public age: number
    function meow(self, content: string): string
        return `{self.name} says {content}`
    end
end
```

However, we have no way to initialize `Cat` without using the default POD syntax.
POD-like constructors `Class { x = x, y = y }` syntax provides a nice default, "named-table" like functionality
that feels great with existing code for POD-like data types. On the other hand, POD-like constructors are not the correct choice
for all flavors of classes. The POD syntax not only allocates a table (fixable in compiler), but there's also no way to customize
our default constructor behavior.

Additionally, a lack of a customizable constructor is a problem if we want to add inheritance in the future.

#### Solution

To fix this issue, we propose a constructor function named `__init`. Among other influences, this is inspired
by the similarly-named `__init__` from Python. This constructor's name is also chosen to match the `__init` proposed in upstream Luau.

When `Class(...args)` syntax is used to invoke the class constructor, the "magic box self allocator" in C allocates an uninitialized object of the class and passes it to `Class.__init(self, ...args)` as `self`. All of `self`'s fields are initially `nil` at runtime irrespective of type annotations.

Once called, the `__init` function *should* then assign to all needed fields in `self` (not checked at runtime), and should not return any values. Field `const`ness is not enforced between initial allocation and when the `Class()` expression finishes evaluation.
Any values returned by `__init` will be ignored. The `Class()` expression then returns `self` to the caller.

If a user forgets to assign to a field in `__init`, a type error `"TypeError: constructor does not initialize property <name>"` is raised, but at runtime the field will be `nil`.

#### The Default (POD) Constructor

To facilitate POD-like behavior, the default `__init` implementation will accept a POD-like table of fields.

If a class does not explicitly define a constructor, it is given a default constructor. The default constructor takes a mapping from property name to value and initializes the newly-created object with those fields.

```luau
class Point
    x: number
    y: number
end

local p = Point { x = 3, y = 4 }
```

There is no runtime check on fields passed to the default constructor: if no argument or `nil` are passed, the default constructor simply initializes all class fields to `nil`. If the table does not specify all fields, the fields left unspecified will be initialized to `nil`. In either case, we will raise a type error in static analysis specifying the incorrect argument or missing fields.

The default constructor is always `public`, and there is no way to mark it as private without explicitly
redefining its semantics.

The default constructor is a real function just like any other and so it can be explicitly invoked if desired.

```luau
class Point
    public x: number
    public y: number

    function reset(self)
        -- note this modifies `self` in place, it doesn't allocate a new self
        self:__init { x = 0, y = 0 }
    end
end
```

#### `public` and `private` constructors

Users can define the `__init` constructor as `public` or `private`.

If the `__init` constructor is `private`, then the class must be created via a special factory function and cannot
be instantiated otherwise.

```luau
class User
    public first_name: string
    public last_name: string
    private ssn: string?

    --- A public constructor which initializes the public and private fields.
    public function __init(self, first: string, last: string, ssn: string?) 
        self.first_name = first
        self.last_name = last
        self.ssn = ssn
    end

    public function name(self): string
        return self.first_name .. " " .. self.last_name
    end

    private function get_ssn(self): string
        if self.ssn then
            return self.ssn
        end
        return get_ssn_from_files(self)
    end
end

const user = User("Taz", "Parekh", "126-222-1123")
```

If a class has a `private` constructor, but no function in the class instantiates an object from that `private` constructor, a type error is raised:

```luau
-- TypeError: this class can never be instantiated because its `__init` constructor is private and is never called; did you mean to return an instance of this class from a `public function` instead? Call the constructor to silence.
class User
    public first_name: string
    public last_name: string
    private ssn: string?

    private function __init(self, first, last, ssn)
        self.first_name = first
        self.last_name = last
        self.ssn = ssn
    end

    public function name(self): string
        return self.first_name .. " " .. self.last_name
    end

    private function get_ssn(self): string
        if self.ssn then
            return self.ssn
        end
        return get_ssn_from_files(self)
    end
end
```

Attempting to initialize an object of a class with a `private` constructor outside of its class will raise a runtime error.

By restricting the constructor, a class can require its users to construct it with factory functions that respect the class's
specific invariants.

```luau
class User
    public id: string
    public first_name: string
    public last_name: string
    private ssn: string?

    -- Cannot be directly accessed using User() outside this class scope
    private function __init(self, first: string, last: string, ssn: string?) 
        self.first_name = first
        self.last_name = last
        self.ssn = ssn
    end

    public function new(id: string): User | Error<string>
        const ssn_for_user = ssns.get(id)
        if typeof(ssn_for_user) == "Error" then
            return Error.new<<string>>(tostring(ssn_for_user))
        end
        const username = usernames.from_id(id)
        return User(username.first, username.last, ssn_for_user)
    end

    public function name(self): string
        return self.first_name .. " " .. self.last_name
    end

    private function get_ssn(self): string
        if self.ssn then
            return self.ssn
        end
        return get_ssn_from_files(self)
    end
end

const user = User.new("12311")
```

### Hoisting

A previous version of this RFC allowed hoisting at runtime, where a `class` binding could be visible to code and classes above it, as long as the class was initialized before the code calling it ran. We have decided to not support this behavior because it is confusing and would be harder to implement. Instead, we're investigating ways to allow for explicit forward declaration of a class so that it can be used in classes defined above it, but said syntax is not yet specified in this RFC.

### Type System

Class declarations blocks introduce the class's type into the type environment.

Unlike table types, class/object types are nominally typed; this means two different
classes with identical members are treated as distinct types and unrelated to one another.

Due to the difficulty of doing so, we choose not to try and infer the type of unannotated class fields.
Any such unannotated fields are typed as `any`.
This is fine because users will likely either provide type annotations in this position or not care about static analysis at all.

When a user uses the class's identifier name to annotate a variable, we annotate the variable as an `object` of the class, not the class
itself.

```luau
class Cat end
-- cats is an array-like table of Cat objects, not an array-like table of multiple copies of the Cat class
const cats: { Cat } = {}
```

Each class is a singleton instance of an unnamed type. If you want to use the class's type instead of the object type, use `typeof(Class)` instead.

The `class.isinstance` function participates in refinement:

```luau
function foo(p: unknown)
    if class.isinstance(p, Point) then
        return {p.x, p.y} -- no error here
    end
end
```

Attempting to access a private member from outside the class raises a TypeError:

```luau
class User
    private do_not_use_this_or_i_get_fired: unknown
    -- ...
    private function terminate(self)
    -- ...
    end
end

const user = User.new("deviaze")
-- TypeError: Field 'do_not_use_this_or_i_get_fired' of class 'User' is private; accessing it here will raise a runtime error
print(user.do_not_use_this_or_i_get_fired)

-- TypeError: Method 'terminate' of class 'User' is private; calling it from here will always raise a runtime error
user:terminate()
```

Classes with generic type parameters should be handled like extern types with generic type parameters. An initial implementation
of this RFC without full type system support may be merged before handling this perfectly.

Attempting to modify a `const` field should raise a type error.

## C API

### `int lua_isclass(lua_State* L, int idx);`

Returns 1 if the value at the index is a class.

### `int lua_isobject(lua_State* L, int idx);`

Returns 1 if the value at the index is an object.

### `void* lua_newobject(lua_State* L, int idx);`

Places a new `object` from the `class` at the index and calls the class's `__init` constructor with all the field values on the stack between top and the index. Places the new `object` on top of the stack.

### `void* lua_newclass(lua_State* L, size_t sz);`

Places a new `class` object with the data size `sz` on top of the stack.

### `const char* lua_getclassname(lua_State* L, int idx)`

Returns the class name (the name of the identifier the class binding was declared with) of the class or object at `idx`. If the value at `idx` is an `object`, follows the class pointer to find the class that object is an instance of.

## Drawbacks

- Implementing classes in a different way from upstream Roblox's Luau may lead to inconsistencies between future code written for upstream Luau vs our Luau. We feel the less complex semantics and implementation of our version of classes (not forcing `.new`, more explicit semantics) is a better long-term goal for the language.
- Extreme complexity of this feature when simpler implementations (only POD), sugar around metatable OOP, etc. could exist
- Private fields throwing a runtime error on access violation imposes performance drawbacks that will need to be worked around in the compiler and VM.

## Alternatives

- We could remove the half-implemented classes from Luau
- Remove `public` keyword and have everything public (no access specifiers, no `const`)
- Add a `static` keyword for fields and methods (instead of requiring users use upvalues for static and `self` meaning method)
- We could remove the half-implemented classes feature and instead add syntactical sugar for the canonical metatable OOP pattern
- We could opt for the old [records proposal by Arseny](https://github.com/luau-lang/luau/pull/205/changes) instead
- Private fields can be enforced only in typechecking and not raise runtime errors
- Implement shared self even with its limitations
- Add syntactical sugar for the current metatable OOP pattern.
- We could instead have "structs" and "implementations" instead of classes and keep things simpler.
- Omit `__init`, and just increase performance of POD table constructor without reserving `new`
- Implement classes exactly as upstream Luau does to maintain compatibility, at the price of choosing a more confusing feature design for no real benefit.

An example of an alternative design of structs and impls instead of classes:

```luau
struct Cat (
    name: string,
    age: number
) implements {
    function meow(self): string
        print(`{self.name} says meow!`)
    end,
}

struct Dog(name, age, puppies)

-- constructor not a table for performance reasons, 
-- argument order chosen by struct field order which is known because this is new syntax 
-- and only the stuff after implements can be an actual table
const cat = Cat("Taz", 12)
```

## Future work

- Add composition, interfaces, inheritance if we want to.
- Syntax sugar for `__init` constructors
