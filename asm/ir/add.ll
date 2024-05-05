; ModuleID = 'point_addition'

; Define the structure representing a point
%Point = type { i32, i32 }

; Function to add two points
define void @add_points(%Point* %point1, %Point* %point2, %Point* %result) {
entry:
  ; Load the values of the points
  %x1 = getelementptr inbounds %Point, %Point* %point1, i32 0, i32 0
  %x1_val = load i32, i32* %x1
  %y1 = getelementptr inbounds %Point, %Point* %point1, i32 0, i32 1
  %y1_val = load i32, i32* %y1

  %x2 = getelementptr inbounds %Point, %Point* %point2, i32 0, i32 0
  %x2_val = load i32, i32* %x2
  %y2 = getelementptr inbounds %Point, %Point* %point2, i32 0, i32 1
  %y2_val = load i32, i32* %y2

  ; Calculate the sum of x and y coordinates
  %x_sum = add nsw i32 %x1_val, %x2_val
  %y_sum = add nsw i32 %y1_val, %y2_val

  ; Store the result in the result point
  %x_result = getelementptr inbounds %Point, %Point* %result, i32 0, i32 0
  store i32 %x_sum, i32* %x_result
  %y_result = getelementptr inbounds %Point, %Point* %result, i32 0, i32 1
  store i32 %y_sum, i32* %y_result

  ret void
}
