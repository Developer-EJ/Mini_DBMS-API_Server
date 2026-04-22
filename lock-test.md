# Lock Test

## Build

```bash
make sqlpd sqlpd_nolock
```

## Run Race Test

```bash
TOTAL=300 WORKERS=8 BATCH=100 ./test_atomicity.sh
```

락이 제거된 `sqlpd_nolock`에서는 `SELECT 불일치`가 1 이상 나오면 동시성 문제가 재현된 것이다.
락이 있는 `sqlpd`는 `SELECT 불일치=0`이 나와야 한다.

예상 형태:

```text
락 없음 요청=  300  라인=  300  고유 id=  300  HTTP 실패=  0  JSON 실패=  0  SELECT 불일치= 17
락 있음 요청=  300  라인=  300  고유 id=  300  HTTP 실패=  0  JSON 실패=  0  SELECT 불일치=  0
```

## Server + Client Crash Test

터미널 1에서 lock 없는 서버를 먼저 띄운다.

```bash
rm -f data/users_nolock.dat
./sqlpd_nolock 18081 64 8192
```

터미널 2에서 클라이언트 요청을 동시에 많이 보낸다.

```bash
PIDS=()
for i in $(seq 1 5000); do
  curl -s -o /dev/null -X POST "http://localhost:18081/sql" \
    -d "sql=INSERT INTO users_nolock VALUES ($i, 'user$i', $((20 + i % 50)), 'u$i@test.com')" &
  PIDS+=("$!")

  if [ $((i % 1000)) -eq 0 ]; then
    for pid in "${PIDS[@]}"; do
      wait "$pid"
    done
    PIDS=()
  fi
done

for pid in "${PIDS[@]}"; do
  wait "$pid"
done
```

터미널 2에서 결과가 꼬였는지 확인한다.

```bash
for i in $(seq 1 100); do
  curl -s --get --data-urlencode "sql=SELECT * FROM users_nolock WHERE id = $i;" \
    "http://localhost:18081/sql"
done
```

서버가 죽으면 crash 재현이고, 안 죽어도 `SELECT WHERE id = n` 결과가 다른 id의 row를 보여주면 lock 없는 쓰기 경쟁 문제가 재현된 것이다.

더 세게 보내려면 서버는 그대로 두고 터미널 2에서 이 값을 키운다.

```bash
for i in $(seq 1 20000); do
  curl -s -o /dev/null -X POST "http://localhost:18081/sql" \
    -d "sql=INSERT INTO users_nolock VALUES ($i, 'user$i', $((20 + i % 50)), 'u$i@test.com')" &
done
wait
```
