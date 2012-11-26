
create table t (i integer);

copy t with source ExternalSource(cmd='seq 1 10') no commit;

copy t with source ExternalSource(cmd='exit 7') no commit;

copy t with source ExternalSource(cmd='echo "message on stderr" >&2') no commit;

copy t with source ExternalSource(cmd='echo "message on stderr and exit" >&2; exit 8') no commit;

copy t with source ExternalSource(cmd='echo "message on stderr" >&2; sleep 0.5; echo "bit later message" >&2') no commit;

copy t with source ExternalSource(cmd='echo "message on stderr" >&2; sleep 5; echo "lot later message" >&2') no commit;

drop table t;
