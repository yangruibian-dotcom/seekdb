"""SQL + HTTP regression checks against a BUILT seekdb and our local fixture.

Requires PyMySQL. Uses a dedicated disposable database; all AI evaluation runs
inside seekdb SQL. The Python code only creates fixtures and checks results.
"""
import argparse
import json
import os
import uuid
from urllib.request import Request, urlopen
from mock_ai_service import embedding


def main():
    import pymysql
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--mock-url", default="http://127.0.0.1:8000")
    args = parser.parse_args()
    conn = pymysql.connect(host=args.host, port=args.port, user=args.user,
                           password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                           charset="utf8mb4", autocommit=True)
    prefix = "ai_reg_" + uuid.uuid4().hex[:10]
    models, endpoints = [], []

    def query(sql, params=None):
        with conn.cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchall()

    def scalar(sql, params=None):
        return query(sql, params)[0][0]

    def rpc(operation, name, value=None):
        if value is None:
            query(f"CALL DBMS_AI_SERVICE.{operation}(%s)", (name,))
        else:
            query(f"CALL DBMS_AI_SERVICE.{operation}(%s,%s)", (name, json.dumps(value)))

    def model(suffix, kind, provider, route, key="fixture-key"):
        name = prefix + suffix
        rpc("CREATE_AI_MODEL", name, {"type": kind, "model_name": "arbitrary-" + suffix})
        models.append(name)
        endpoint = name + "_ep"
        rpc("CREATE_AI_MODEL_ENDPOINT", endpoint, {"ai_model_name": name,
            "provider": provider, "url": args.mock_url + route, "access_key": key})
        endpoints.append(endpoint)
        return name, endpoint

    def requests(reset=False):
        req = Request(args.mock_url + "/requests", method="DELETE" if reset else "GET")
        with urlopen(req, timeout=5) as response:
            return json.load(response)

    def expect_error(sql, params):
        try:
            query(sql, params)
        except pymysql.MySQLError:
            assert scalar("SELECT 1") == 1, "connection unusable after error"
        else:
            raise AssertionError("expected SQL error: " + sql)

    try:
        query(f"CREATE DATABASE {prefix}")
        query(f"USE {prefix}")
        complete, complete_ep = model("_c", "completion", "OpenAI", "/v1/chat/completions")
        embed, embed_ep = model("_e", "dense_embedding", "openai", "/v1/embeddings")
        rerank, rerank_ep = model("_r", "rerank", "siliconflow", "/v1/rerank")
        ollama, ollama_ep = model("_o", "completion", "oLlAmA", "/api/generate", "")
        oe, _ = model("_oe", "dense_embedding", "ollama", "/api/embed", "")
        for provider in ("Ollama", "OLLAMA", "ollama"):
            rpc("ALTER_AI_MODEL_ENDPOINT", ollama_ep, {"provider": provider})
            assert scalar("SELECT provider FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE endpoint_name=%s", (ollama_ep,)) == provider
            assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'provider-only'))", (ollama,)) == "provider-only"
        rpc("ALTER_AI_MODEL_ENDPOINT", ollama_ep, {"url": args.mock_url + "/api/generate"})
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'empty-key-retained'))", (ollama,)) == "empty-key-retained"
        expect_error("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT(%s,%s)", (ollama_ep, json.dumps({"provider":"OPENAI"})))
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'still-ollama'))", (ollama,)) == "still-ollama"
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'雪',NULL))", (complete,)) == "雪"
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}/{0}', 'abc')))", (complete,)) == "abc/abc"
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}',AI_PROMPT('<{0}>','雪'))))", (complete,)) == "<雪>"
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}',JSON_ARRAY(1,2))))", (complete,)) == "[1, 2]"
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}/{0}/{1}',JSON_ARRAY(1,true),JSON_OBJECT('k',2))))", (complete,)) == '[1, true]/[1, true]/{"k": 2}'
        escaped = '雪\\\n"'
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT('{0}',%s)))", (complete,escaped)) == escaped
        for template in ('{1}', '{-1}', '{18446744073709551616}'):
            expect_error("SELECT AI_COMPLETE(%s,AI_PROMPT(%s,'x'))", (complete,template))
        query("SELECT AI_COMPLETE(%s,'config',JSON_OBJECT('temperature',0.5,'max_tokens',7))", (complete,))
        body = requests()[-1]["body"]
        assert body["temperature"] == 0.5 and body["max_tokens"] == 7
        query("CREATE TABLE texts(id INT PRIMARY KEY, body LONGTEXT, template TEXT)")
        with conn.cursor() as cur:
            cur.executemany("INSERT INTO texts VALUES(%s,%s,%s)",
                [(i, None if i % 9 == 0 else "row-" + str(i), "{0}/{0}") for i in range(150)])
        query("INSERT INTO texts VALUES(200,%s,'{0}')", ("长" * 70000,))
        assert scalar("SELECT CHAR_LENGTH(JSON_UNQUOTE(AI_COMPLETE(%s,AI_PROMPT(template,body)))) FROM texts WHERE id=200", (complete,)) == 70000
        requests(reset=True)
        rows = query("SELECT id,AI_EMBED(%s,body,7) FROM texts WHERE id<150", (embed,))
        assert len(rows) == 150
        for row, vector in rows:
            assert (vector is None) if row % 9 == 0 else (json.loads(vector) == embedding("row-" + str(row), 7))
        calls = [x["body"]["input"] for x in requests() if x["route"] == "/v1/embeddings"]
        assert any(len(x) > 1 for x in calls), "SQL engine did not use cross-row batching"
        assert len(calls) >= 3 and sum(map(len, calls)) == 133
        rows = query("SELECT id,AI_EMBED(%s,body) FROM texts WHERE id BETWEEN 4 AND 40 ORDER BY id LIMIT 11", (embed,))
        assert [r[0] for r in rows] == list(range(4,15))
        for row, vector in rows:
            assert (vector is None) if row % 9 == 0 else (json.loads(vector) == embedding("row-" + str(row), 4))
        for dimension in (0, -1, None, 1.5):
            expect_error("SELECT AI_EMBED(%s,'a',%s)", (embed, dimension))
        documents = [{"id": i + 100, "text": "x" * (i % 7 + 1)} for i in range(45)]
        result = json.loads(scalar("SELECT AI_RERANK(%s,'q',%s,'text')", (rerank, json.dumps(documents))))
        expected = sorted(documents, key=lambda x: -len(x["text"]))
        assert [x["id"] for x in result] == [x["id"] for x in expected]
        result = json.loads(scalar("SELECT AI_RERANK(%s,'q',%s)", (rerank, json.dumps([x["text"] for x in documents]))))
        assert [x["index"] for x in result] == [x["id"] - 100 for x in expected]
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'native'))", (ollama,)) == "native"
        assert json.loads(scalar("SELECT AI_EMBED(%s,'native')", (oe,))) == embedding("native", 4)
        rpc("ALTER_AI_MODEL_ENDPOINT", ollama_ep, {"provider": "OPENAI", "access_key": "fixture-key", "url": args.mock_url + "/v1/chat/completions"})
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'changed'))", (ollama,)) == "changed"
        rpc("ALTER_AI_MODEL_ENDPOINT", ollama_ep, {"provider": "OLLAMA", "access_key": "", "url": args.mock_url + "/api/generate"})
        assert scalar("SELECT JSON_UNQUOTE(AI_COMPLETE(%s,'back'))", (ollama,)) == "back"
        for fault in ("empty", "null", "root", "http"):
            rpc("ALTER_AI_MODEL_ENDPOINT", complete_ep, {"url": args.mock_url + f"/fault/{fault}/v1/chat/completions"})
            expect_error("SELECT AI_COMPLETE(%s,'a')", (complete,))
        for fault in ("duplicate", "missing", "dimension", "element", "root", "http"):
            rpc("ALTER_AI_MODEL_ENDPOINT", embed_ep, {"url": args.mock_url + f"/fault/{fault}/v1/embeddings"})
            expect_error("SELECT AI_EMBED(%s,body,7) FROM texts WHERE id BETWEEN 1 AND 8", (embed,))
        rpc("ALTER_AI_MODEL_ENDPOINT", embed_ep, {"url": args.mock_url + "/v1/embeddings"})
        assert json.loads(scalar("SELECT AI_EMBED(%s,'recovered')", (embed,))) == embedding("recovered", 4)
        for fault in ("duplicate", "missing", "root", "http"):
            rpc("ALTER_AI_MODEL_ENDPOINT", rerank_ep, {"url": args.mock_url + f"/fault/{fault}/v1/rerank"})
            expect_error("SELECT AI_RERANK(%s,'q',%s)", (rerank, json.dumps(["a", "b"])))
        print("PASS: SQL semantics, batching/request counts, row mapping, rerank IDs, provider changes and failure recovery")
    finally:
        for endpoint in reversed(endpoints):
            rpc("DROP_AI_MODEL_ENDPOINT", endpoint)
        for name in reversed(models):
            rpc("DROP_AI_MODEL", name)
        query(f"DROP DATABASE IF EXISTS {prefix}")
        conn.close()


if __name__ == "__main__":
    main()
