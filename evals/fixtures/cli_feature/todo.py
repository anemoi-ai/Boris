"""A tiny to-do list. Usage: python3 todo.py list | add TITLE | done ID"""
import argparse
import json

TASKS_FILE = "tasks.json"


def load():
    with open(TASKS_FILE) as f:
        return json.load(f)


def save(tasks):
    with open(TASKS_FILE, "w") as f:
        json.dump(tasks, f, indent=2)


def main(argv=None):
    parser = argparse.ArgumentParser(prog="todo")
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    add = sub.add_parser("add")
    add.add_argument("title")
    done = sub.add_parser("done")
    done.add_argument("id", type=int)
    args = parser.parse_args(argv)

    tasks = load()
    if args.cmd == "list":
        for t in tasks:
            print(f"[{'x' if t['done'] else ' '}] {t['id']}. {t['title']}")
    elif args.cmd == "add":
        tasks.append({"id": max((t["id"] for t in tasks), default=0) + 1,
                      "title": args.title, "done": False})
        save(tasks)
    elif args.cmd == "done":
        for t in tasks:
            if t["id"] == args.id:
                t["done"] = True
        save(tasks)


if __name__ == "__main__":
    main()
