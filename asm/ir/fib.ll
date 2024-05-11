@str = constant [4 x i8] c"%d\0A\00" ; "%d\n\00" Null-terminated

declare i32 @printf(i8*, ...)

define i32 @fib(i32 %n) {
entry:
    %n_leq_1 = icmp sle i32 %n, 1
    br i1 %n_leq_1, label %return_n, label %recurse

recurse:
    %n_minus_1 = sub i32 %n, 1
    %fib_n_minus_1 = call i32 @fib(i32 %n_minus_1)
    %n_minus_2 = sub i32 %n, 2
    %fib_n_minus_2 = call i32 @fib(i32 %n_minus_2)
    %result = add i32 %fib_n_minus_1, %fib_n_minus_2
    br label %return

return_n:
    br label %return

return:
    %fib_n = phi i32 [%n, %return_n], [%result, %recurse]
    ret i32 %fib_n
}

define i32 @main() {
    %n = call i32 (i32) @fib(i32 42)
    %str_ptr = bitcast [4 x i8]* @str to i8*
    call i32 (i8*, ...) @printf(i8* %str_ptr, i32 %n)
    ret i32 0
}