Nano Script Specification
--------------------------

> Version: 2023.12

# Design Goal 
- Small size, both code and runtime
- Embeddable
- Easy to learn & use
- Open source

# Encoding format
- Only ascii characters are allowed in source code.
- IEEE 754 double precision floating point number is used for number literal.

# Grammar Notation
- All terminal symbols are write in sans-serif font `like this`.
- All non-terminal symbols are write in serif font *like this*.
- $A^n$ is sequence of $n$ iterations of A.
- $A^*$ is sequence of zero or more iterations of A.
- $A^+$ is sequence of one or more iterations of A.
- $A^?$ is an optional A.
- Productions are written *sync ::= A_1 | ... | A_n;*

# Auxiliary Notation
- *e* denote the empty sequence.
- *|s|* denote the length of sequence *s*.
- *s[i]* denote the *i*th element of sequence *s*.
- *s[i:n]* denote the subsequence of *s from [i] to [i + n - 1]* of a sequence *s*.
