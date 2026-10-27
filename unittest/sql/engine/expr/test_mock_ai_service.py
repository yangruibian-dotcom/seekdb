import json
import threading
import unittest
from urllib.request import Request, urlopen
from mock_ai_service import MockServer, embedding


class TestMock(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = MockServer(("127.0.0.1", 0))
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def post(self, route, **body):
        req = Request(f"http://127.0.0.1:{self.server.server_port}" + route,
                      data=json.dumps({"model": "arbitrary", **body}).encode(),
                      headers={"Content-Type": "application/json"})
        with urlopen(req, timeout=5) as response:
            return json.load(response)

    def test_reverse_embeddings(self):
        value = self.post("/v1/embeddings", input=["雪", "b"], dimensions=7)
        self.assertEqual([x["index"] for x in value["data"]], [1, 0])
        self.assertEqual(value["data"][1]["embedding"], embedding("雪", 7))

    def test_ollama(self):
        self.assertEqual(self.post("/api/generate", prompt="hello", stream=False)["response"], "hello")
        self.assertEqual(len(self.post("/api/embed", input=["a"])["embeddings"][0]), 4)

    def test_fault(self):
        self.assertEqual(self.post("/fault/root/v1/embeddings", input=["a"]), [])
        value = self.post("/fault/element/v1/embeddings", input=["a"])
        self.assertEqual(value["data"][0]["embedding"][0], "bad")

    def test_rerank_ties(self):
        value = self.post("/v1/rerank", query="q", documents=["aa", "bb", "c"])
        self.assertEqual([x["index"] for x in value["results"]], [2, 1, 0])
        self.assertEqual(value["results"][1]["relevance_score"], value["results"][2]["relevance_score"])


if __name__ == "__main__":
    unittest.main()
