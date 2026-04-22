# Mini DBMS API Server Benchmark Commands

이 문서는 `ab`로 READ/WRITE, 싱글 worker/멀티 worker를 비교하기 위한 명령어 모음이다.

서버는 터미널 A에서 켜고, `ab`는 터미널 B에서 실행한다. 서버 종료는 터미널 A에서 `Ctrl+C`.

## 0. 공통 준비

```bash
# 빌드
make

# READ 테스트용 20만 row 생성
# 기존 data/users.dat를 덮어쓴다.
make seed_users
```

# 20만 rows 생성

```
make seed_users
```

```
ab -n 100 -c 10 "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%3B"
```
# 데이터 초기화
```
make clear_users
```
# 원하는 row 수로 생성
```
make seed_users ROWS=10000
```

## 1. READ 테스트 - 싱글 worker

터미널 A:

```bash
./sqlpd 8080 1 128
```

터미널 B:

```bash
# point read 부하 테스트
ab -n 1000 -c 100 \
  "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%3B"
```

## 2. READ 테스트 - 멀티 worker

터미널 A:

```bash
./sqlpd 8080 64 128
```

터미널 B:

```bash
# 워밍업 1회
curl -s "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20%3D%20500000%3B" > /dev/null

# point read 부하 테스트
ab -n 10000 -c 100 \
  "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20%3D%20500000%3B"
```

## 3. WRITE 테스트 - 싱글 worker

터미널 B에서 먼저 데이터와 body 파일 준비:

```bash
# 데이터 초기화
make clear_users

# INSERT body 파일 생성
printf "INSERT INTO users VALUES (1, 'bench_user1', 21, 'bench_user1@example.com');" > /tmp/insert.sql
```

터미널 A:

```bash
./sqlpd 8080 1 128
```

터미널 B:

```bash
#초기화
make clear_users

# write 부하 테스트
ab -n 10000 -c 100 \
  -p /tmp/insert.sql \
  -T "text/plain" \
  http://127.0.0.1:8080/sql

# INSERT row 수 확인
wc -l data/users.dat
```


## 4. WRITE 테스트 - 멀티 worker

터미널 B에서 먼저 데이터와 body 파일 준비:

```bash
# 데이터 초기화
make clear_users

# INSERT body 파일 생성
printf "INSERT INTO users VALUES (1, 'bench_user1', 21, 'bench_user1@example.com');" > /tmp/insert.sql
```

터미널 A:

```bash
./sqlpd 8080 8 128
```

터미널 B:

```bash
# write 부하 테스트
ab -n 100000 -c 100 \
  -p /tmp/insert.sql \
  -T "text/plain" \
  http://127.0.0.1:8080/sql

# INSERT row 수 확인
wc -l data/users.dat
```

## 5. 선택: READ range query 테스트

터미널 A:

```bash
# 싱글 worker로 보려면 1, 멀티 worker로 보려면 8
./sqlpd 8080 8 128
```

터미널 B:

```bash
# 워밍업
curl -s "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20BETWEEN%20500000%20AND%20501000%3B" > /dev/null

# range read 부하 테스트
ab -n 2000 -c 100 \
  "http://127.0.0.1:8080/sql?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20BETWEEN%20500000%20AND%20501000%3B"
```

## 결과에서 볼 지표

```text
Requests per second
Time per request
Failed requests
50% / 95% / 99%
100% longest request
```

## 실험 표 양식

```text
READ point query, rows=200,000, ab=-n 10000 -c 100

workers | RPS | avg latency | p50 | p95 | p99 | failed
--------|-----|-------------|-----|-----|-----|-------
1       |     |             |     |     |     |
8       |     |             |     |     |     |


WRITE insert, ab=-n 100000 -c 100

workers | RPS | avg latency | p50 | p95 | p99 | failed
--------|-----|-------------|-----|-----|-----|-------
1       |     |             |     |     |     |
8       |     |             |     |     |     |
```
