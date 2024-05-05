declare i32 @printf(i8*, ...)

@hello_world_string = constant [14 x i8] c"Hello, world!\00"

define i32 @main() {
entry:
  ; Get the format string pointer
  %format_string = getelementptr inbounds [14 x i8], [14 x i8]* @hello_world_string, i32 0, i32 0
  
  ; Call printf
  call i32 (i8*, ...) @printf(i8* %format_string)

  ret i32 0
}