#ifndef USER_H
#define USER_H

#include <QString>

namespace shiftnet {

class User
{
public:
    enum Group {
        Unknown,
        Administrator,
        Member,
        Guest
    };

    inline User()
        : _id(0)
        , _duration(0)
        , _group(Unknown)
    {}

    inline static User createGuest(int duration)
    { return User(0, "Tamu", duration, Guest); }

    inline static User createAdministrator()
    { return User(0, "Administrator", 0, Administrator); }

    inline static User createMember(uint id, const QString& username, int duration)
    { return User(id, username, duration, Member); }

    inline int id() const { return _id; }
    inline QString username() const { return _username; }
    inline Group group() const { return _group; }
    inline int duration() const { return _duration; }

    inline void addDuration(int minute) { _duration += minute; }

    inline bool isUnknown() const { return group() == Unknown; }
    inline bool isAdmin() const { return group() == Administrator; }
    inline bool isGuest() const { return group() == Guest; }
    inline bool isMember() const { return group() == Member; }

private:
    User(int id, const QString& username, int duration, Group group)
        : _id(id), _duration(duration), _group(group), _username(username) {}

    uint _id;
    uint _duration;
    Group _group;
    QString _username;
};

}

#endif // USER_H
