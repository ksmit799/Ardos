import unittest


class TestSanity(unittest.TestCase):
    def setUp(self):
        self.answer = 4

    def test_sanity(self):
        self.assertEqual(2 + 2, self.answer)


if __name__ == '__main__':
    unittest.main()
