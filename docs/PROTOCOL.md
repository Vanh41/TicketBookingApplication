# Giao thức TCP và lệnh CLI

Client dòng lệnh và server giao tiếp bằng JSON Lines UTF-8 trên TCP IPv4. Server mặc định dùng cổng `3636`. Bảng bên dưới ánh xạ cả 14 action sang lệnh CLI, quyền và dữ liệu trả về. [Bảng lệnh CLI](../src/client/ticket_cli.c#L21) · [Bảng điều phối server](../src/server/request_handler.c#L22)

## Định dạng thông điệp

Mỗi yêu cầu và phản hồi là một đối tượng JSON trên một dòng, kết thúc bằng LF (`\n`). Giới hạn là **4.096 byte tính cả LF**. `ticket_read_line()` giữ lại byte của dòng tiếp theo nếu một lần `recv()` nhận nhiều dòng, và tiếp tục đọc nếu dòng hiện tại chưa kết thúc. Một dòng quá dài được bỏ qua đến LF, sau đó server trả `MESSAGE_TOO_LARGE` và có thể tiếp tục đọc dòng sau. [Đọc và gửi thông điệp](../src/common/ticket_protocol.c#L10)

Client gửi ba trường `id`, `action` và `data`. `id` là số nguyên từ 1 đến 2.147.483.647 trên các nền tảng được dùng cho đồ án. `action` phân biệt hoa thường. `data` luôn là một object, có thể là `{}` khi action không có tham số. Ví dụ yêu cầu xem suất số 2 như sau.

```json
{"id":15,"action":"GET_SEATS","data":{"showing_id":2}}
```

Phản hồi thành công có `status: "OK"` và object `data`. Ví dụ với suất ở phòng số 3 chưa có ghế nào được đặt như sau.

```json
{"id":15,"status":"OK","data":{"layout":"6x10","booked":[],"showing":{"theater_id":3,"starts_at":"2030-01-15 19:30"}}}
```

Phản hồi lỗi có `status: "ERROR"` và object `error` chứa `code` và `message`. Nếu server đọc được ID hợp lệ, nó trả lại đúng ID đó kể cả khi action lỗi. Lỗi khi không lấy được ID hợp lệ hoặc khi dòng vượt giới hạn trả `id: null`. [Đóng gói phản hồi](../src/server/request_handler.c#L90)

```json
{"id":16,"status":"ERROR","error":{"code":"SEAT_UNAVAILABLE","message":"Seat A2 is already booked."}}
```

Mỗi kết nối mới là một phiên Guest. Gửi `LOGIN` và các yêu cầu sau đăng nhập trên cùng socket. CLI đợi phản hồi sau mỗi lệnh. Client riêng có thể gửi nhiều dòng liên tiếp. Server xử lý tuần tự các yêu cầu trong một kết nối và phản hồi theo cùng thứ tự. `id` dùng để đối chiếu yêu cầu với phản hồi và log. [Vòng lặp server](../src/server/main.c#L20) · [Trao đổi của CLI](../src/client/ticket_cli.c#L363)

## Bảng action

Các trường trong ngoặc vuông là tùy chọn. Với ba lệnh nhận mật khẩu, CLI hỏi riêng nếu bỏ đối số mật khẩu trong terminal tương tác. Khi chạy qua stdin từ script, CLI yêu cầu đối số mật khẩu trong dòng lệnh ứng dụng. `help`, `exit` và `quit` được xử lý tại client.

Khi server trả một phản hồi `ERROR` đúng cấu trúc, `exchange()` in mã lỗi rồi CLI tiếp tục đọc lệnh. Lỗi gửi, đóng kết nối khi đang chờ phản hồi hoặc phản hồi sai cấu trúc làm CLI dừng với mã thoát khác 0. Script dùng CLI cần đọc kết quả của từng lệnh. [Xử lý phản hồi](../src/client/ticket_cli.c#L387) · [Mã thoát](../src/client/main.c#L71)

| Action và lệnh CLI | Quyền | Trường trong `data` của yêu cầu | `data` khi thành công |
| --- | --- | --- | --- |
| `REGISTER` · `register USER [PASSWORD]` | Guest | `username`, `password` | `user_id`. Tài khoản mới có quyền `BUYER`. |
| `LOGIN` · `login USER [PASSWORD]` | Guest | `username`, `password` | `user_id`, `role` |
| `LOGOUT` · `logout` | Buyer, Manager, Admin | `{}` | `{}` |
| `SEARCH_MOVIES` · `search "TITLE" [after_id=ID]` | Buyer, Manager | `title`, [`after_id`] | `movies`, [`next_after_id`] |
| `BROWSE_MOVIES` · `browse` | Buyer, Manager | [`genre`], [`location`], [`date`], [`time`], [`after_id`] | `showings`, [`next_after_id`] |
| `GET_SHOWINGS` · `showings MOVIE_ID [after_id=ID]` | Buyer, Manager | `movie_id`, [`after_id`] | `showings`, [`next_after_id`] |
| `GET_SEATS` · `seats SHOWING_ID` | Buyer | `showing_id` | `layout`, `booked`, `showing` |
| `BOOK_TICKETS` · `book SHOWING_ID SEAT [SEAT ...]` | Buyer | `showing_id`, `seats`, [`expected_showing`] | `booking_id` |
| `LIST_THEATERS` · `theaters [after_id=ID]` | Manager | [`after_id`] | `theaters`, [`next_after_id`] |
| `ADD_MOVIE` · `add-movie "TITLE" "GENRE" MINUTES "DESCRIPTION"` | Manager | `title`, `genre`, `duration`, `description` | `movie_id` |
| `ADD_SHOWING` · `add-showing MOVIE_ID THEATER_ID "YYYY-MM-DD HH:MM" [...]` | Manager | `movie_id`, `slots` gồm 1–10 object có `theater_id`, `starts_at` | `showing_ids` theo thứ tự đầu vào |
| `UPDATE_SHOWING` · `update-showing SHOWING_ID theater_id=ID` hoặc `starts_at="YYYY-MM-DD HH:MM"` | Manager | `showing_id` và ít nhất một trong hai trường `theater_id`, `starts_at` | `showing` gồm `id`, `theater_id`, `starts_at` |
| `CREATE_USER` · `create-user USER ROLE [PASSWORD]` | Admin | `username`, `password`, `role` | `user_id` |
| `SET_ROLE` · `set-role USER_ID ROLE` | Admin | `user_id`, `new_role` | `user_id`, `role` |

## Quy tắc dữ liệu

- **Tài khoản.** Username dài 3–20 ký tự, dùng chữ cái ASCII, chữ số hoặc dấu gạch dưới. Mật khẩu dài 6–32 ký tự và không chứa khoảng trắng. Giá trị role gửi qua giao thức là `BUYER`, `MANAGER` hoặc `ADMIN`. Quyền của từng action được kiểm tra độc lập, nên Admin chỉ gọi được những action có Admin trong bảng. [Kiểm tra tài khoản](../src/server/ticket_auth.c#L15)
- **Tìm phim và duyệt suất.** `SEARCH_MOVIES` tìm chuỗi con trong tên, không phân biệt hoa thường và giữ phân biệt dấu. `BROWSE_MOVIES` so khớp thể loại và địa điểm theo toàn bộ chuỗi, không phân biệt hoa thường. Ngày và giờ so khớp chính xác theo định dạng `YYYY-MM-DD` và `HH:MM`. Các bộ lọc kết hợp theo AND. Một thể loại hoặc địa điểm chưa có trong danh mục trả `INVALID_FILTER`. Tập rỗng là kết quả hợp lệ khi các bộ lọc hợp lệ nhưng không có suất khớp. CLI nhận dạng `key=value`, chẳng hạn `browse genre="Phiêu lưu" location="Da Nang" time=19:30`. [Tìm và lọc](../src/server/ticket_catalog.c#L80) · [So khớp chuỗi](../src/server/ticket_db.c#L53)
- **Thêm phim và xếp lịch.** Tên dài 1–100 ký tự, thể loại 1–30, mô tả 0–500 và thời lượng là số nguyên 1–500 phút. `starts_at` dùng `YYYY-MM-DD HH:MM` với ngày hợp lệ và giờ 24 tiếng. Một yêu cầu `UPDATE_SHOWING` có thể gửi cả phòng lẫn giờ. [Kiểm tra và thêm phim](../src/server/ticket_catalog.c#L201) · [Kiểm tra ngày giờ](../src/server/ticket_db.c#L179)
- **Sơ đồ và ghế.** `layout` là chuỗi dạng `"6x10"`. `booked` là mảng mã ghế đã đặt. `showing` trong `GET_SEATS` có `theater_id` và `starts_at`. `seats` trong `BOOK_TICKETS` có 1–10 mã riêng biệt nằm trong sơ đồ. Mã gồm chữ cái in hoa chỉ hàng và số cột, ví dụ `A1` hoặc `F10`. CLI chuyển chữ cái đầu sang in hoa. Để đặt theo thông tin vừa xem, gửi object `showing` từ `GET_SEATS` làm `expected_showing` của cùng suất. CLI tự làm bước này khi đã lưu sơ đồ của suất. [Đọc sơ đồ và kiểm tra ghế](../src/server/ticket_booking.c) · [Lưu thông tin suất trên CLI](../src/client/ticket_cli.c#L363)

## Dữ liệu danh sách và phân trang

| Mảng | Trường trên mỗi phần tử |
| --- | --- |
| `movies` | `id`, `title`, `genre`, `duration` |
| `showings` | `id`, `movie_id`, `title`, `genre`, `cinema`, `theater`, `location`, `starts_at` |
| `theaters` | `id`, `cinema`, `location`, `theater`, `rows`, `columns` |

Ở trang đầu, bỏ `after_id`. Khi có `next_after_id`, giữ nguyên action và bộ lọc, rồi gửi giá trị đó làm `after_id` ở trang tiếp theo. Mỗi trang sắp xếp tăng theo ID, chứa tối đa 30 dòng và có thể ngắn hơn do giới hạn kích thước. Trang cuối bỏ trường `next_after_id`. CLI in cursor để người dùng tự lấy trang sau. [Phân trang server](../src/server/ticket_catalog.c#L29) · [Hiển thị danh sách](../src/client/ticket_cli.c#L295)

## Mã lỗi

| Mã | Khi nào xuất hiện |
| --- | --- |
| `INVALID_REQUEST` | Envelope hoặc tham số sai kiểu, thiếu trường hoặc ngoài giới hạn. |
| `MESSAGE_TOO_LARGE` | Dòng yêu cầu vượt 4.096 byte tính cả LF. |
| `UNKNOWN_ACTION` | Tên action không có trong bảng điều phối. |
| `NOT_LOGGED_IN` | Phiên Guest gọi action yêu cầu đăng nhập. |
| `PERMISSION_DENIED` | Role không được phép gọi action, hoặc yêu cầu muốn hạ quyền Admin cuối. |
| `INVALID_CREDENTIALS` | Tên đăng nhập hoặc mật khẩu không khớp. |
| `USERNAME_TAKEN` | Username đã tồn tại. |
| `INVALID_FILTER` | Bộ lọc duyệt suất sai hoặc thể loại hay địa điểm chưa có trong danh mục. |
| `NOT_FOUND` | ID hợp lệ về định dạng nhưng bản ghi phim, phòng, suất hoặc người dùng được yêu cầu không tồn tại. |
| `SHOWING_CONFLICT` | Suất tạo hoặc sửa chồng thời gian với suất khác trong cùng phòng. |
| `SHOWING_HAS_BOOKINGS` | Yêu cầu sửa lịch của suất đã có lượt đặt. |
| `SHOWING_CHANGED` | Phòng hoặc giờ hiện tại khác `expected_showing`. Mở lại sơ đồ trước khi xác nhận lựa chọn mới. |
| `SEAT_UNAVAILABLE` | Một ghế đã có người đặt. Toàn bộ lượt hiện tại được rollback. |
| `INTERNAL_ERROR` | Lỗi phía server, chẳng hạn SQLite không thể hoàn tất giao dịch. |

Xem [các bảo đảm khi xử lý đồng thời và cách xác định lỗi](RELIABILITY.md) để biết cách đọc log và kiểm tra trạng thái sau lỗi.
